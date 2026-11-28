"""Grouped e2e cases: orders, boss, spell_miss, raid_25man."""

import types
from wlblib import wire
from wlblib.testing import ProxyRecorder as _ProxyRecorder, expect, find_option, send_cmd

def _case_orders():
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


    # Uses shared `wlblib.testing.ProxyRecorder` (`_ProxyRecorder`).


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
                            if parent.orders_for_loada:
                                for o in parent.orders_for_loada:
                                    lines.append(f"order: {o}")
                            else:
                                lines.append("orders: none")
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
                def wait_plan(prev_count, prev_choose_count=None):
                    if prev_choose_count is None:
                        with proxy.lock:
                            prev_choose_count = sum(
                                1 for _, c in proxy.commands
                                if c.get("bot") == "Loada" and "choose" in c
                            )
                    deadline = time.monotonic() + 10.0
                    while time.monotonic() < deadline:
                        with brain.lock:
                            replied = brain.loada_replies > prev_count
                        with proxy.lock:
                            cur_chooses = sum(
                                1 for _, c in proxy.commands
                                if c.get("bot") == "Loada" and "choose" in c
                            )
                        if replied and cur_chooses > prev_choose_count:
                            with brain.lock:
                                return brain.loada_replies
                        time.sleep(0.05)
                    expect(False, "Loada never installed a new plan (reply + choose)")

                def act_cmds(since_idx=0):
                    return [
                        (i, t, c) for i, (t, c) in enumerate(proxy.commands)
                        if i >= since_idx and c.get("bot") == "Loada" and "act" in c
                    ]

                # A cast refused for a moment (the GCD or a cooldown playerbots'
                # own cast just started) is timing, not a broken order.
                transient = re.compile(
                    r"cannot cast .* now|is on cooldown|spell is not ready yet"
                )

                def check_no_errors():
                    with proxy.lock:
                        expect(not proxy.visibility_errors,
                               f"visibility errors: {proxy.visibility_errors}")
                        errs = [
                            ev["event"]["error"] for _, ev in proxy.events
                            if "error" in ev.get("event", {})
                            and not transient.search(ev["event"]["error"].get("text", ""))
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
                with proxy.lock:
                    latest_loada = next(
                        s for _, s in reversed(proxy.snapshots) if s.get("bot") == "Loada"
                    )
                expect("Renew" in _auras_on_self(latest_loada),
                       "Renew expired before edge-latch check completed")
                extra_acts = act_cmds(behind_entry[0] + 1)
                expect(not extra_acts,
                       f"order re-fired while condition stayed true: {extra_acts}")

                # Restating the exact same order on a new plan while Renew is still
                # active must keep the latch and NOT re-fire (High #1).
                prev_replies = brain.set_orders([
                    "Renew on me -> away group 10, then to behind Telda",
                ])
                replan_cmd_mark = len(proxy.commands)
                server.console("llmbrain whisper Telda Loada restate", wait_s=0.1, inst=TEST)
                wait_plan(prev_replies)
                time.sleep(1.2)
                with proxy.lock:
                    latest_loada = next(
                        s for _, s in reversed(proxy.snapshots) if s.get("bot") == "Loada"
                    )
                expect("Renew" in _auras_on_self(latest_loada),
                       "Renew expired before restate-latch check completed")
                restate_acts = act_cmds(replan_cmd_mark)
                expect(not restate_acts,
                       f"restated order re-fired while Renew held: {restate_acts}")

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
                def _targeted_boar_marked_skull(p):
                    for _, s in reversed(p.snapshots):
                        if s.get("bot") != "Loada":
                            continue
                        tgt = (s.get("ext", {}).get("target", {}).get("target") or {})
                        if tgt.get("name") != "Mottled Boar":
                            continue
                        tguid = tgt.get("guid", "")
                        if tgt.get("raid_marker") == "SKULL":
                            return s
                        if tguid and any(
                            u.get("guid") == tguid and u.get("raid_marker") == "SKULL"
                            for u in s.get("base", {}).get("nearby", {}).get("units", [])
                        ):
                            return s
                    return None

                marked_snap = proxy.wait_until(
                    _targeted_boar_marked_skull,
                    timeout_s=10.0,
                    desc="snapshot showing targeted Mottled Boar marked SKULL",
                )
                expect(marked_snap is not None, "targeted Mottled Boar was never marked SKULL")
                server.console("llmtest unaura Loada 139", wait_s=0.3, inst=TEST)
                check_no_errors()

                # ---- 3. `use` (Power Word: Shield through client handler) ------------
                prev_replies = brain.set_orders([
                    "Renew on me & not Power Word: Shield on me -> use Power Word: Shield on me",
                ])
                server.console("llmbrain whisper Telda Loada phase3", wait_s=0.1, inst=TEST)
                wait_plan(prev_replies)
                shield_snap = None
                for attempt in range(2):
                    # Strip any Power Word: Shield / Weakened Soul playerbots may
                    # have cast in combat so `not Power Word: Shield on me` holds.
                    server.console("llmtest unaura Loada 17", wait_s=0.1, inst=TEST)
                    server.console("llmtest unaura Loada 6788", wait_s=0.1, inst=TEST)
                    cmd_mark = len(proxy.commands)
                    server.console("llmtest aura Loada 139", wait_s=0.1, inst=TEST)
                    try:
                        # No act when playerbots' own Shield landed between the unaura
                        # and the Renew (`not Power Word: Shield on me` false): retried.
                        proxy.wait_until(
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
                                 if s.get("bot") == "Loada"
                                 and "Power Word: Shield" in _auras_on_self(s)),
                                None,
                            ),
                            timeout_s=4.0,
                            desc="Power Word: Shield aura on Loada",
                        )
                        break
                    except AssertionError:
                        # Playerbots may have just triggered GCD (e.g. a buff or
                        # combat cast), causing HandleCastSpellOpcode to refuse PW:S.
                        if attempt == 1:
                            raise
                        server.console("llmtest unaura Loada 139", wait_s=1.8, inst=TEST)
                expect(shield_snap is not None, "Power Word: Shield never appeared on Loada")
                server.console("llmtest unaura Loada 139", wait_s=0.3, inst=TEST)
                server.console("llmtest unaura Loada 17", wait_s=0.2, inst=TEST)
                # Clear any transient GCD refusal from attempt 0 before checking
                # errors for the rest of the run.
                with proxy.lock:
                    proxy.events = [
                        (t, ev) for t, ev in proxy.events
                        if "cannot cast Power Word: Shield now"
                        not in ev.get("event", {}).get("error", {}).get("text", "")
                    ]
                check_no_errors()

                # ---- 4. `kite` (> 5 s without timeout + new plan stops kite) ---------
                prev_replies = brain.set_orders([
                    "Renew on me -> kite nearest Mottled Boar",
                ])
                with brain.lock:
                    wake_mark = len(brain.loada_wake_reasons)
                with proxy.lock:
                    choose_mark = sum(1 for _, c in proxy.commands
                                      if c.get("bot") == "Loada" and "choose" in c)
                server.console("llmbrain whisper Telda Loada phase4", wait_s=0.1, inst=TEST)
                # A wake left over from phase 3 (a refused Shield: "is on cooldown") can
                # install the kite plan first; the phase4 whisper's own wake must land
                # before kiting starts, or it is counted as a wake while kiting.
                deadline = time.monotonic() + 10.0
                while time.monotonic() < deadline:
                    with brain.lock:
                        if any("whisper from Telda" in w
                               for w in brain.loada_wake_reasons[wake_mark:]):
                            break
                    time.sleep(0.05)
                replies_before_kite = wait_plan(prev_replies, choose_mark)
                with brain.lock:
                    wakes_before_kite = len(brain.loada_wake_reasons)
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
                # Let Loada kite for 6.0 s (> the 5 s default step timeout) and check
                # movement continued past the midpoint (not just moved once and froze).
                time.sleep(6.0)
                mid_t = kite_entry[1] + 3.0
                with proxy.lock:
                    first_half = [
                        _pos2d(s) for t, s in proxy.snapshots
                        if s.get("bot") == "Loada" and kite_entry[1] <= t < mid_t
                    ]
                    second_half = [
                        _pos2d(s) for t, s in proxy.snapshots
                        if s.get("bot") == "Loada" and t >= mid_t
                    ]
                kite_positions = first_half + second_half
                max_kite_disp = max((math.dist(start_pos, p) for p in kite_positions), default=0.0)
                expect(max_kite_disp >= 5.0,
                       f"kite moved Loada at most {max_kite_disp:.1f} yd from {start_pos}")
                expect(len(second_half) >= 2 and math.dist(second_half[0], second_half[-1]) >= 1.5,
                       f"kite froze in second half: {second_half[:2]}..{second_half[-2:]}")
                with brain.lock:
                    timeout_wakes = [w for w in brain.loada_wake_reasons if "timed out" in w]
                    # Kiting can run Loada into another boar: a combat wake is the world's,
                    # not the kite timing out, and allowed (the full e2e run's intermittent
                    # failure). Any other wake while kiting is not.
                    kite_wakes = brain.loada_wake_reasons[wakes_before_kite:]
                    other_wakes = [w for w in kite_wakes
                                   if not ("entered combat" in w or "attacked" in w)]
                    expect(not timeout_wakes and not other_wakes,
                           f"unexpected wake while kiting: {kite_wakes}")

                # Sending a new plan while kiting (Renew still on) must emit `stop moving`
                # (High #2).
                stop_mark = len(proxy.commands)
                prev_replies = brain.set_orders([])
                server.console("llmbrain whisper Telda Loada stopkite", wait_s=0.1, inst=TEST)
                wait_plan(prev_replies)
                proxy.wait_until(
                    lambda _: next(
                        ((i, t, c) for i, t, c in act_cmds(stop_mark)
                         if c["act"].get("verb") == "STOP" and c["act"].get("stop") == "MOVING"),
                        None,
                    ),
                    timeout_s=8.0,
                    desc="STOP MOVING act command when new plan drops kite",
                )
                server.console("llmtest unaura Loada 139", wait_s=0.3, inst=TEST)
                check_no_errors()

                # ---- 5. Higher-priority order preempts running lower-priority one ----
                prev_replies = brain.set_orders([
                    "Renew on me -> target Telda",
                    "Power Word: Shield on me -> kite nearest Mottled Boar",
                ])
                server.console("llmbrain whisper Telda Loada phase5", wait_s=0.1, inst=TEST)
                wait_plan(prev_replies)
                cmd_mark = len(proxy.commands)
                server.console("llmtest aura Loada 17", wait_s=0.1, inst=TEST)
                proxy.wait_until(
                    lambda _: next(
                        ((i, t, c) for i, t, c in act_cmds(cmd_mark)
                         if c["act"].get("verb") == "KITE"),
                        None,
                    ),
                    timeout_s=8.0,
                    desc="lower-priority KITE act command",
                )
                preempt_mark = len(proxy.commands)
                server.console("llmtest aura Loada 139", wait_s=0.1, inst=TEST)
                proxy.wait_until(
                    lambda _: next(
                        ((i, t, c) for i, t, c in act_cmds(preempt_mark)
                         if c["act"].get("verb") == "TARGET" and c["act"].get("name") == "Telda"),
                        None,
                    ),
                    timeout_s=8.0,
                    desc="higher-priority TARGET Telda preempting KITE",
                )
                server.console("llmtest unaura Loada 139", wait_s=0.2, inst=TEST)
                server.console("llmtest unaura Loada 17", wait_s=0.2, inst=TEST)
                check_no_errors()

                # ---- 6. Refused act returns error event and wakes brain --------------
                prev_replies = brain.set_orders([
                    "Renew on me -> use Shadow Word: Pain on focus",
                ])
                server.console("llmbrain whisper Telda Loada phase6", wait_s=0.1, inst=TEST)
                replies_before_fail = wait_plan(prev_replies)
                server.console("llmtest aura Loada 139", wait_s=0.1, inst=TEST)
                wait_plan(replies_before_fail)
                with brain.lock:
                    fail_wakes = [w for w in brain.loada_wake_reasons if "standing order failed" in w]
                    expect(bool(fail_wakes),
                           f"refused act never woke brain: {brain.loada_wake_reasons}")
                server.console("llmtest unaura Loada 139", wait_s=0.2, inst=TEST)

                return (f"away {away_dist:.1f} yd -> behind Telda ({behind_dist:.1f} yd) + restate-latch, "
                        f"marked boar SKULL, shielded self, kited {max_kite_disp:.1f} yd + replan-stop, "
                        f"preempt, refused-act wake")
            finally:
                harness_proc.terminate()
                try:
                    harness_proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    harness_proc.kill()
                brain.stop()
                proxy.stop()
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_boss():
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


    NAME = "boss"
    DESCRIPTION = "live boss plumbing (Loken, HoL): Lightning Nova cast -> away Loken 25"
    TEST_CONFIG = "boss"
    BOT = "Loada"
    ONLINE = 2

    LOKEN_X = 1186.47
    LOKEN_Y = 33.8305
    NOVA_RADIUS_YD = 20.0
    # Loken's Lightning Nova is a 5.0 s normal (4.0 s heroic) cast with a 20 yd
    # blast radius; from point-blank melee (~0.5 yd) at 7 yd/s run speed,
    # clearing with >= 1000 ms remaining proves an early dodge, not a last-second one.
    MIN_DODGE_MARGIN_MS = 1000
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


    def _loken_cast_remaining_ms(loken):
        if not loken:
            return None
        act_obj = loken.get("activity", {})
        cast_obj = act_obj.get("cast", {}) if isinstance(act_obj, dict) else {}
        if not isinstance(cast_obj, dict) or not cast_obj.get("spell"):
            return None
        return int(cast_obj.get("remaining_ms", 0))


    def _loken_dist(snap, loken):
        if loken and isinstance(loken.get("rel"), dict) and "dist_yd" in loken["rel"]:
            return float(loken["rel"]["dist_yd"])
        x, y = _pos2d(snap)
        return math.hypot(x - LOKEN_X, y - LOKEN_Y)


    # Uses shared `wlblib.testing.ProxyRecorder` (`_ProxyRecorder`).


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

        # Gear both priests at level 80, group them, and place Telda as tank near
        # Loken (1186.47, 33.83) with Loada ~12 yd West (inside Loken's 20 yd
        # Lightning Nova danger radius) in Halls of Lightning (map 602), so Loada
        # always runs out West down the open corridor in clear LoS of Loken.
        server.console("llmtest gear Loada 80", wait_s=0.5, inst=TEST)
        server.console("llmtest gear Telda 80", wait_s=0.5, inst=TEST)
        server.console("llmtest maketank Telda", wait_s=0.5, inst=TEST)
        server.console("llmtest group Loada Telda", wait_s=0.5, inst=TEST)
        server.console("llmtest tele Loada 602 1174.0 33.83 60.81 0.0", wait_s=0.5, inst=TEST)
        server.console("llmtest tele Telda 602 1182.0 31.83 60.81 0.0", wait_s=1.0, inst=TEST)

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
                # A healer can already stand 25 yd out when the Nova starts: then `away 25`
                # is done at once and there is nothing to move (Loken may close in a little).
                moved = dist_after >= dist_start + 8.0 or dist_start >= 25.0
                if not (moved and dist_after >= NOVA_RADIUS_YD):
                    with proxy.lock:
                        tl = [
                            (round(t - t_nova_start, 2),
                             round(_loken_dist(s, _find_loken(s)), 1),
                             _loken_cast_remaining_ms(_find_loken(s)),
                             tuple(round(v, 1) for v in _pos2d(s)),
                             (s.get("base", {}).get("self", {}).get("casting") or {}).get("spell", {}).get("name", ""),
                             s.get("done_act_command_id"))
                            for t, s in proxy.snapshots
                            if s.get("bot") == "Loada" and t >= t_nova_start - 0.5
                        ]
                    expect(False,
                           f"Loada only moved {dist_start:.1f} -> {dist_after:.1f} yd from Loken; tl={tl[:25]}")

                # 5. Dodge safety margin (Gap 5): Loada starts inside the 20 yd
                # Lightning Nova radius and must clear 20 yd while the cast still has at
                # least MIN_DODGE_MARGIN_MS remaining (no last-second dodges).
                expect(
                    dist_start < NOVA_RADIUS_YD,
                    f"setup: Loada started {dist_start:.1f} yd from Loken (outside "
                    f"{NOVA_RADIUS_YD:.0f} yd danger radius)",
                )
                with proxy.lock:
                    clear_snap = next(
                        (s for t, s in proxy.snapshots
                         if s.get("bot") == "Loada"
                         and t >= t_nova_start
                         and _loken_dist(s, _find_loken(s)) >= NOVA_RADIUS_YD),
                        None,
                    )
                clear_loken = _find_loken(clear_snap) if clear_snap else None
                clear_cast = _loken_cast_name(clear_loken)
                clear_rem_ms = (
                    _loken_cast_remaining_ms(clear_loken)
                    if clear_cast == "Lightning Nova"
                    else 0
                )
                expect(
                    clear_snap is not None
                    and clear_cast == "Lightning Nova"
                    and clear_rem_ms is not None
                    and clear_rem_ms >= MIN_DODGE_MARGIN_MS,
                    f"last-second dodge: cleared {NOVA_RADIUS_YD:.0f} yd with "
                    f"{clear_rem_ms} ms left on cast {clear_cast!r} "
                    f"(need >= {MIN_DODGE_MARGIN_MS} ms; dist_start={dist_start:.1f})",
                )

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

                margin_str = f", margin {clear_rem_ms} ms" if clear_rem_ms is not None else ""
                return (f"Loken Lightning Nova: react {react_ms:.0f} ms, "
                        f"dist {dist_start:.1f} -> {dist_after:.1f} yd{margin_str}, 0 deaths")
            finally:
                harness_proc.terminate()
                try:
                    harness_proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    harness_proc.kill()
                brain.stop()
                proxy.stop()
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)


def _case_spell_miss():
    """Live spell-miss e2e test (M2.2) on the isolated test server.

    Runs the real C++ `out/common/harness run` against two grouped level-80
    priests (`Loada` and `Telda`) next to Mottled Boars, then broadcasts a
    scripted WoW 3.3.5a `SMSG_SPELL_GO` miss packet (`llmtest spellmiss`) from
    `Loada` to every player in visibility range. Verifies:
      1. `Loada` receives `self.spell_miss` in a reactive snapshot.
      2. `Loada`'s harness wakes the brain with `Woken: missed: ...` and renders
         `missed: Shadow Word: Pain on Mottled Boar (resist)` on her screen.
      3. A second consecutive miss of the exact same spell increments `seq` and
         wakes `Loada` again.
      4. Bystander `Telda` (1 yd away, receiving the same broadcast packet)
         never gets `spell_miss` in her snapshot and never wakes on a miss.
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

    NAME = "spell_miss"
    DESCRIPTION = "scripted SMSG_SPELL_GO miss surfaces on caster screen and wakes brain; bystander ignores broadcast"
    TEST_CONFIG = "orders"
    BOT = "Loada"
    ONLINE = 2

    KEEP_RE = re.compile(r"^\s+(\d+)\. Keep doing what I'm doing$", re.M)

    class _MockBrainServer:
        def __init__(self):
            self.lock = threading.Lock()
            self.prompts = {"Loada": [], "Telda": []}
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
                    m = KEEP_RE.search(user_text)
                    keep_opt = int(m.group(1)) if m else 1
                    for bot_name in ("Loada", "Telda"):
                        if user_text.startswith(f"You are {bot_name}."):
                            with parent.lock:
                                parent.prompts[bot_name].append(user_text)
                            break
                    reply = json.dumps({
                        "choices": [{"message": {"content": "\n".join([
                            f"do: {keep_opt}",
                            "expect: none",
                            "watch: none",
                            "look: 300",
                        ])}}],
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

        server.console("llmtest move Telda -669.0 -4282.0 41.0 0.0", wait_s=0.3, inst=TEST)
        server.console("llmtest move Loada -668.0 -4282.0 41.0 0.0", wait_s=0.3, inst=TEST)
        server.console("llmtest group Loada Telda", wait_s=0.5, inst=TEST)

        with tempfile.TemporaryDirectory(prefix="wlb-e2e-spellmiss-") as tmp:
            tmp_dir = pathlib.Path(tmp)
            bots_dir = tmp_dir / "bots"
            proxy_sock = tmp_dir / "proxy.sock"
            proxy = _ProxyRecorder(stack.socket, proxy_sock)
            brain = _MockBrainServer()
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
                def wait_prompts(bot_name, count, desc):
                    deadline = time.monotonic() + 10.0
                    while time.monotonic() < deadline:
                        with brain.lock:
                            if len(brain.prompts[bot_name]) >= count:
                                return list(brain.prompts[bot_name])
                        time.sleep(0.05)
                    expect(False, f"timed out waiting for {desc}")

                # Wait for both bots' initial wakes so subsequent wakes are reactive.
                wait_prompts("Loada", 1, "Loada initial wake")
                wait_prompts("Telda", 1, "Telda initial wake")
                with brain.lock:
                    loada_before = len(brain.prompts["Loada"])

                expected_line = "missed: Shadow Word: Pain on Mottled Boar (resist)"

                # 1. Broadcast SMSG_SPELL_GO (589 = Shadow Word: Pain, 2 = SPELL_MISS_RESIST).
                server.console("llmtest spellmiss Loada 589 2", wait_s=0.1, inst=TEST)

                first_snap = proxy.wait_until(
                    lambda p: next(
                        (s for _, s in reversed(p.snapshots)
                         if s.get("bot") == "Loada"
                         and s.get("base", {}).get("self", {}).get("spell_miss")),
                        None,
                    ),
                    timeout_s=8.0,
                    desc="Loada snapshot with self.spell_miss",
                )
                miss1 = first_snap["base"]["self"]["spell_miss"]
                expect(miss1.get("spell") == "Shadow Word: Pain", f"unexpected spell: {miss1}")
                expect(miss1.get("target") == "Mottled Boar", f"unexpected target: {miss1}")
                expect(miss1.get("reason") == "resist", f"unexpected reason: {miss1}")
                seq1 = miss1.get("seq", 0)
                expect(seq1 >= 1, f"expected seq >= 1, got {miss1}")

                prompts1 = wait_prompts("Loada", loada_before + 1, "Loada wake on first spell miss")
                wake_prompt1 = next(
                    (p for p in prompts1[loada_before:] if expected_line in p),
                    None,
                )
                expect(wake_prompt1 is not None,
                       f"expected {expected_line!r} in Loada wake prompt; got {prompts1[loada_before:]}")
                expect(f"Woken: {expected_line}." in wake_prompt1,
                       f"expected wake reason for miss in prompt:\n{wake_prompt1}")

                # 2. Second consecutive identical miss must bump seq and wake Loada again.
                with brain.lock:
                    loada_before_2 = len(brain.prompts["Loada"])
                server.console("llmtest spellmiss Loada 589 2", wait_s=0.1, inst=TEST)

                second_snap = proxy.wait_until(
                    lambda p: next(
                        (s for _, s in reversed(p.snapshots)
                         if s.get("bot") == "Loada"
                         and (s.get("base", {}).get("self", {}).get("spell_miss") or {}).get("seq", 0) > seq1),
                        None,
                    ),
                    timeout_s=8.0,
                    desc="Loada snapshot with incremented spell_miss.seq",
                )
                seq2 = second_snap["base"]["self"]["spell_miss"]["seq"]
                prompts2 = wait_prompts("Loada", loada_before_2 + 1, "Loada wake on second spell miss")
                expect(any(f"Woken: {expected_line}." in p for p in prompts2[loada_before_2:]),
                       f"second identical miss did not wake Loada: {prompts2[loada_before_2:]}")

                # 3. Adversarial: bystander Telda received the broadcast SMSG_SPELL_GO
                # too and must NOT record spell_miss or wake on it.
                time.sleep(1.0)
                with proxy.lock:
                    telda_misses = [
                        s.get("base", {}).get("self", {}).get("spell_miss")
                        for _, s in proxy.snapshots
                        if s.get("bot") == "Telda"
                        and s.get("base", {}).get("self", {}).get("spell_miss")
                    ]
                    expect(not telda_misses,
                           f"bystander Telda leaked caster's spell_miss: {telda_misses}")
                    expect(not proxy.visibility_errors,
                           f"visibility errors: {proxy.visibility_errors}")
                with brain.lock:
                    telda_miss_wakes = [
                        p for p in brain.prompts["Telda"] if "missed:" in p
                    ]
                    expect(not telda_miss_wakes,
                           f"bystander Telda woke on caster's miss: {telda_miss_wakes}")

                return f"caster woke twice (seq {seq1}->{seq2}) with {expected_line!r}; bystander Telda ignored"
            finally:
                harness_proc.terminate()
                try:
                    harness_proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    harness_proc.kill()
                brain.stop()
                proxy.stop()
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_raid_25man():
    """25-man raid fight e2e with boss notes, phases/vehicle notes, and M8/M9/M11 invariants (M17).

    Runs the real C++ `out/common/harness run` with curated `--notes-dir=content/notes` against
    a 25-player Alliance raid (24 bots `Loada`..`Loadx` across all 4 role buckets: 2 tanks,
    5 healers, 8 melee DPS, 9 ranged DPS + 1 real scripted human client `Allyraid25`) inside
    Icecrown Citadel (map 631), using a deterministic local mock LLM server so it runs
    reliably in `wlb test e2e` without competing for `llama-server`:
      1. Raid body strategies (M8): verifies `+icc` is preserved on combat + noncombat while
         brain-led (for Gunship vehicles/cannons & positioning) and `grind`/`new rpg` are off.
      2. 8k prompt budget & guaranteed sections (M9): inspects real HTTP prompts sent by the
         C++ harness at 25-man raid size (24 other raid members in `ext.raid` / `ext.party`),
         verifying every prompt fits within the 8192-token slot budget and retains `Woken:`,
         curated ICC boss notes, and leader raid chat.
      3. Shared per-role standing order compilation (M9): verifies standing orders compile
         per role bucket (`tank`, `healer`, `melee`, `ranged`) with cache sharing across peers
         (`<< 24` compile calls + `orders cache hit` / `shared with` in harness logs).
      4. `/rw spread` & `/rw stack` (M11): verifies all 22 non-tank raid bots spread out with
         25-man spacing and stack back, while the 2 tanks (`Loada`, `Loadb`) hold boss position
         and do NOT apply unaddressed spread/stack.
      5. Multi-phase / Gunship add & Festergut boss mechanics + non-tank raid debuff visibility
         (M11):
         - Phase 1 (ICC Gunship Battle add): spawns a passive `Skybreaker Sorcerer` (37116);
           verifies multiple raid bots (including shared-cache peers) fire `TARGET Skybreaker
           Sorcerer` standing-order acts.
         - Phase 2 (Festergut boss + raid debuff): spawns a passive `Festergut` (36626) so the
           harness dynamically selects `## Festergut` notes and compiles `Gas Spore on me -> to
           group`; applies `Gas Spore` (69279) on non-tank ranged DPS `Loadp` and verifies BOTH
           non-tank raid debuff visibility in other raid members' `ext.party.members` snapshots
           AND `Loadp` firing a `TO GROUP` standing-order act.
    """
    import http.server
    import json
    import math
    import os
    import pathlib
    import re
    import subprocess
    import tempfile
    import threading
    import time

    from wlblib import build, server, wowclient
    from wlblib.common import OUT, ROOT, TEST, say
    from wlblib.testing import expect

    NAME = "raid_25man"
    DESCRIPTION = "25-man ICC raid: 8k prompt budget, shared role orders, raid debuffs, spread/stack, phases"
    TEST_CONFIG = "raid25"
    BOT = "Loada"
    ONLINE = 24

    BOTS = tuple(f"Load{chr(ord('a') + i)}" for i in range(ONLINE))
    TANKS = ("Loada", "Loadb")
    NON_TANKS = BOTS[2:]
    SPECS = {
        # 2 tanks: Prot Paladin, Prot Warrior.
        "Loada": 1, "Loadb": 2,
        # 5 healers: Disc Priest, Holy Priest, Resto Shaman, Resto Druid, Holy Paladin.
        "Loadc": 0, "Loadd": 1, "Loade": 2, "Loadf": 2, "Loadg": 0,
        # 8 melee DPS: Rogues, Warriors, Ret Paladins.
        "Loadh": 1, "Loadi": 0, "Loadj": 1, "Loadk": 0,
        "Loadl": 2, "Loadm": 2, "Loadn": 1, "Loado": 1,
        # 9 ranged DPS: Mages, Warlocks, Hunters, Shadow Priest, Elem Shaman, Balance Druid.
        "Loadp": 2, "Loadq": 1, "Loadr": 0, "Loads": 2,
        "Loadt": 1, "Loadu": 0, "Loadv": 2, "Loadw": 0, "Loadx": 0,
    }
    HUMAN = "Allyraid"
    ACCOUNT = "HUMANRAID25"
    NORTHSHIRE = "0 -8949.95 -132.49 83.53 0"
    ICC_STAGE = "631 76.86 2211.37 30.0 3.15"
    STAGE_XY = (76.86, 2211.37)
    LOADP_OUT_POS = "631 62.86 2211.37 30.0 0.0"
    SKYBREAKER_SORCERER = 37116
    FESTERGUT = 36626
    GAS_SPORE_SPELL = 69279
    KEEP_RE = re.compile(r"^\s+(\d+)\. Keep doing what I'm doing$", re.M)
    NAME_RE = re.compile(r"^You are (\w+)\.", re.M)
    SLOT_BUDGET_TOKENS = 8192

    def _poll(pred, timeout_s, what, interval=0.3):
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            val = pred()
            if val:
                return val
            time.sleep(interval)
        expect(False, f"timed out ({timeout_s} s) waiting for {what}")

    def _ensure_account(account, password):
        def _ready():
            out = subprocess.run(
                [
                    "mysql",
                    f"--defaults-file={OUT / 'server' / 'etc' / 'mysql.cnf'}",
                    "-uroot",
                    "-N",
                    "-e",
                    f"select id from {TEST.db_prefix}auth.account where username = '{account}'",
                ],
                capture_output=True,
                text=True,
                check=True,
            ).stdout.strip()
            return bool(out)

        if not _ready():
            server.console(f"account create {account} {password}", wait_s=1.0, inst=TEST)
            _poll(_ready, 20, f"account {account}")
        key = os.urandom(40)
        wowclient.prepare_account(TEST.db_prefix, account, key)
        return key

    class _Raid25MockBrain:
        """Deterministic local LLM stand-in that records full 25-man prompts and compiles
        phase-appropriate standing orders from curated ICC boss notes."""

        def __init__(self):
            self.lock = threading.Lock()
            self.requests = []
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
                    msgs = body.get("messages", [])
                    sys_text = msgs[0].get("content", "") if len(msgs) >= 1 else ""
                    user_text = msgs[1].get("content", "") if len(msgs) >= 2 else ""
                    max_tokens = int(body.get("max_tokens", 0))
                    m_name = NAME_RE.search(user_text)
                    bot = m_name.group(1) if m_name else "?"
                    woken = next((ln for ln in user_text.splitlines() if ln.startswith("Woken: ")), "")
                    is_compile = "compile standing orders from boss notes" in woken
                    m_keep = KEEP_RE.search(user_text)
                    keep_opt = int(m_keep.group(1)) if m_keep else 1

                    lines = [
                        f"do: {keep_opt}",
                        "expect: none",
                        "watch: none",
                        "look: 300",
                    ]
                    if "## Festergut" in user_text and "Gas Spore" in user_text:
                        lines.append("order: Gas Spore on me -> to group")
                    elif "Skybreaker Sorcerer" in user_text:
                        lines.append(
                            "order: Skybreaker Sorcerer appears -> target nearest Skybreaker Sorcerer"
                        )
                    else:
                        lines.append("orders: none")

                    with parent.lock:
                        parent.requests.append({
                            "t": time.monotonic(),
                            "bot": bot,
                            "sys_bytes": len(sys_text.encode("utf-8")),
                            "user_bytes": len(user_text.encode("utf-8")),
                            "max_tokens": max_tokens,
                            "woken": woken,
                            "is_compile": is_compile,
                            "in_icc": "Icecrown Citadel" in user_text,
                            "has_icc_notes": "# Icecrown Citadel" in user_text,
                            "has_festergut": "## Festergut" in user_text,
                            "user_text": user_text,
                        })

                    reply = json.dumps({
                        "choices": [{"message": {"content": "\n".join(lines)}}],
                        "usage": {"prompt_tokens": 1000, "completion_tokens": 30},
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

    def _where(snap):
        return (snap or {}).get("base", {}).get("where", {})

    def _in_icc(snap):
        w = _where(snap)
        return bool(w.get("in_instance")) and "Icecrown Citadel" in w.get("map", "")

    def _pos2(snap):
        p = _where(snap).get("position")
        return (p["x"], p["y"]) if p else None

    def _party_members(snap):
        return ((snap or {}).get("ext") or {}).get("party", {}).get("members", [])

    def run(stack):
        build.common("default", tests=False)
        key = _ensure_account(ACCOUNT, "humanraid25pw")
        for bot in BOTS:
            server.console(f"llmtest gear {bot} 80 {SPECS[bot]}", wait_s=0.15, inst=TEST)
        for tank in TANKS:
            server.console(f"llmtest maketank {tank}", wait_s=0.15, inst=TEST)

        client = None
        with tempfile.TemporaryDirectory(prefix="wlb-e2e-raid25-") as tmp:
            tmp_dir = pathlib.Path(tmp)
            proxy_sock = tmp_dir / "proxy.sock"
            proxy = _ProxyRecorder(stack.socket, proxy_sock)
            brain = _Raid25MockBrain()
            proxy.start()
            brain.start()
            harness_proc = subprocess.Popen(
                [
                    str(OUT / "common" / "harness"),
                    "run",
                    f"--socket={proxy_sock}",
                    f"--rules={ROOT / 'content' / 'RULES.md'}",
                    f"--bots-dir={tmp_dir / 'bots'}",
                    f"--notes-dir={ROOT / 'content' / 'notes'}",
                    "--host=127.0.0.1",
                    f"--port={brain.port}",
                    "--slots=8",
                    "--temperature=0",
                ],
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            harness_log = []

            def _drain():
                if harness_proc.stdout is not None:
                    for line in harness_proc.stdout:
                        harness_log.append(line)

            threading.Thread(target=_drain, daemon=True).start()

            def latest(bot, after=0.0):
                with proxy.lock:
                    return next(
                        (s for t, s in reversed(proxy.snapshots)
                         if s.get("bot") == bot and t >= after),
                        None,
                    )

            def _check_alive():
                expect(
                    harness_proc.poll() is None,
                    f"harness exited early:\n{''.join(harness_log[-30:])}",
                )

            try:
                client = wowclient.WowClient(TEST.world_port, ACCOUNT, key)
                client.authenticate()
                client.enter_world(HUMAN, race=1, cls=1)
                server.console(f"llmtest ungroup {HUMAN}", wait_s=0.2, inst=TEST)
                server.console(f"character level {HUMAN} 80", wait_s=0.3, inst=TEST)
                time.sleep(0.5)

                # Form 25-man raid (human leader + 24 bots across all 4 roles), switch to
                # 25-man normal difficulty so ICC accepts all 25 players, and enter ICC.
                for bot in BOTS:
                    server.console(f"llmtest group {HUMAN} {bot}", wait_s=0.1, inst=TEST)
                client.set_raid_difficulty(wowclient.RAID_DIFFICULTY_25MAN_NORMAL)

                def _raid_25_ready():
                    with client.lock:
                        return client.raid_difficulty == wowclient.RAID_DIFFICULTY_25MAN_NORMAL

                _poll(_raid_25_ready, 10, "25-man raid difficulty confirmation")

                icc_t = time.monotonic()
                server.console(f"llmtest tele {HUMAN} {ICC_STAGE}", wait_s=0.2, inst=TEST)

                def _human_in_icc():
                    with client.lock:
                        return any(w["map"] == 631 and w["t"] >= icc_t for w in client.new_worlds)

                _poll(_human_in_icc, 15, f"{HUMAN} worldport ack into ICC")
                for bot in BOTS:
                    server.console(f"llmtest tele {bot} {ICC_STAGE}", wait_s=0.1, inst=TEST)

                _poll(
                    lambda: all(
                        (s := latest(b, icc_t)) is not None
                        and _in_icc(s)
                        and "raid" in (s.get("ext") or {})
                        and len(_party_members(s)) == 24
                        for b in BOTS
                    ),
                    40,
                    "all 24 bots inside ICC reporting 25-man raid membership",
                )
                say("e2e raid_25man: 25-man raid staged inside Icecrown Citadel")

                # 1. Raid body strategies (M8): `+icc` preserved, `grind`/`new rpg` stripped.
                for check_bot in ("Loada", "Loadc", "Loadh", "Loadp"):
                    icc_strat = server.console(
                        f"llmtest checkstrat {check_bot} icc", wait_s=0.2, inst=TEST
                    )
                    expect(
                        "combat=1" in icc_strat and "noncombat=1" in icc_strat,
                        f"expected +icc body strategy on {check_bot}: {icc_strat.strip()}",
                    )
                grind_strat = server.console("llmtest checkstrat Loadp grind", wait_s=0.2, inst=TEST)
                expect(
                    "noncombat=0" in grind_strat,
                    f"expected grind stripped while brain-led in ICC: {grind_strat.strip()}",
                )

                # 2. Shared per-role order compilation + 8k prompt budget (M9).
                _poll(
                    lambda: (
                        _check_alive() is None
                        and any("cached 1 standing orders for Icecrown Citadel" in ln
                                for ln in list(harness_log))
                        and any(
                            ("shared with" in ln or "orders cache hit for Icecrown Citadel" in ln)
                            for ln in list(harness_log)
                        )
                    ),
                    25,
                    "shared per-role ICC standing order compilation",
                )
                time.sleep(2.0)

                # Send a leader raid chat line and verify it survives into the 25-man prompt.
                leader_msg = "hold center for phase transition"
                chat_t = time.monotonic()
                client.say(wowclient.CHAT_MSG_RAID, leader_msg)
                _poll(
                    lambda: any(
                        r["t"] >= chat_t and leader_msg in r["user_text"]
                        for r in list(brain.requests)
                    ),
                    15,
                    "leader raid chat surviving inside 25-man prompt",
                )

                with brain.lock:
                    icc_reqs = [r for r in brain.requests if r["in_icc"]]
                    phase1_compiles = [
                        r for r in icc_reqs if r["is_compile"] and not r["has_festergut"]
                    ]
                expect(len(icc_reqs) >= 4, f"too few ICC LLM requests recorded: {len(icc_reqs)}")
                expect(
                    len(phase1_compiles) <= 8,
                    f"shared role order compile did not deduplicate across 24 bots: "
                    f"{len(phase1_compiles)} compiles",
                )
                max_est_tokens = 0
                for r in icc_reqs:
                    est_tokens = int((r["sys_bytes"] + r["user_bytes"]) / 3.5) + r["max_tokens"]
                    max_est_tokens = max(max_est_tokens, est_tokens)
                    expect(
                        est_tokens <= SLOT_BUDGET_TOKENS,
                        f"25-man prompt for {r['bot']} exceeded 8k slot budget: "
                        f"est={est_tokens} (sys={r['sys_bytes']}B, user={r['user_bytes']}B, "
                        f"max_tokens={r['max_tokens']})",
                    )
                    expect(bool(r["woken"]), f"25-man prompt for {r['bot']} dropped Woken line")
                    expect(
                        r["has_icc_notes"],
                        f"25-man prompt for {r['bot']} dropped curated ICC boss notes",
                    )
                say(
                    f"e2e raid_25man: 8k prompt budget ok (max_est={max_est_tokens}), "
                    f"shared role compiles={len(phase1_compiles)} for 24 bots"
                )

                # 3. /rw spread & /rw stack with 25-man spacing and tank exclusion (M11).
                log_spread = len(stack.server_log())
                spread_t = time.monotonic()
                client.raid_warning("spread")
                _poll(
                    lambda: all(
                        f"mod-llm-brain: {b} applied leader command spread from {HUMAN}"
                        in stack.server_log()[log_spread:]
                        for b in NON_TANKS
                    ),
                    20,
                    "all 22 non-tank raid bots applying /rw spread",
                )
                spread_log = stack.server_log()[log_spread:]
                for tank in TANKS:
                    expect(
                        f"mod-llm-brain: {tank} applied leader command spread from {HUMAN}"
                        not in spread_log,
                        f"tank {tank} must be excluded from unaddressed /rw spread",
                    )

                def _spread_moved():
                    _check_alive()
                    moved = sum(
                        1 for b in NON_TANKS
                        if (p := _pos2(latest(b, spread_t))) is not None
                        and math.dist(p, STAGE_XY) >= 5.0
                    )
                    return moved >= 14

                _poll(_spread_moved, 20, ">= 14/22 non-tanks spreading >= 5 yd")

                log_stack = len(stack.server_log())
                stack_t = time.monotonic()
                client.raid_warning("stack")
                _poll(
                    lambda: all(
                        f"mod-llm-brain: {b} applied leader command stack from {HUMAN}"
                        in stack.server_log()[log_stack:]
                        for b in NON_TANKS
                    ),
                    20,
                    "all 22 non-tank raid bots applying /rw stack",
                )
                stack_log = stack.server_log()[log_stack:]
                for tank in TANKS:
                    expect(
                        f"mod-llm-brain: {tank} applied leader command stack from {HUMAN}"
                        not in stack_log,
                        f"tank {tank} must be excluded from unaddressed /rw stack",
                    )

                def _stacked_back():
                    _check_alive()
                    near = sum(
                        1 for b in NON_TANKS
                        if (p := _pos2(latest(b, stack_t))) is not None
                        and math.dist(p, STAGE_XY) <= 7.5
                    )
                    return near >= 16

                _poll(_stacked_back, 25, ">= 16/22 non-tanks stacking within 7.5 yd")
                say("e2e raid_25man: /rw spread & /rw stack + tank exclusion verified")

                # 4. Phase 1 (ICC Gunship Battle add): Skybreaker Sorcerer target switch via
                #    shared per-role standing orders.
                p1_t = time.monotonic()
                out_sorc = server.console(
                    f"llmtest marknear {HUMAN} 7 {SKYBREAKER_SORCERER} 12",
                    wait_s=0.3,
                    inst=TEST,
                )
                expect("marked" in out_sorc, f"setup: Skybreaker Sorcerer spawn failed: {out_sorc}")

                def _sorcerer_targeted():
                    _check_alive()
                    with proxy.lock:
                        bots_targeted = {
                            c.get("bot")
                            for t, c in proxy.commands
                            if t >= p1_t
                            and (c.get("act") or {}).get("verb") == "TARGET"
                            and (c.get("act") or {}).get("name") == "Skybreaker Sorcerer"
                        }
                    return bots_targeted if len(bots_targeted) >= 8 else None

                sorc_bots = _poll(
                    _sorcerer_targeted,
                    15,
                    ">= 8 raid bots firing shared TARGET Skybreaker Sorcerer order",
                )

                # 5. Phase 2 (Festergut boss transition + non-tank raid debuff visibility +
                #    `Gas Spore on me -> to group` order).
                p2_t = time.monotonic()
                out_fest = server.console(
                    f"llmtest marknear {HUMAN} 6 {FESTERGUT} 12",
                    wait_s=0.3,
                    inst=TEST,
                )
                expect("marked" in out_fest, f"setup: Festergut spawn failed: {out_fest}")
                ranged_bots = BOTS[15:]
                _poll(
                    lambda: any(
                        r["t"] >= p2_t and r["has_festergut"] and r["bot"] in ranged_bots
                        for r in list(brain.requests)
                    ),
                    20,
                    "Festergut boss notes dynamically selected and compiled",
                )
                time.sleep(1.5)

                # Move non-tank ranged DPS Loadp out and apply Gas Spore (69279).
                server.console(f"llmtest tele Loadp {LOADP_OUT_POS}", wait_s=0.3, inst=TEST)
                spore_t = time.monotonic()
                aura_out = server.console(
                    f"llmtest aura Loadp {GAS_SPORE_SPELL}", wait_s=0.2, inst=TEST
                )
                expect("added aura" in aura_out, f"setup: Gas Spore aura failed: {aura_out}")

                def _other_sees_nontank_debuff():
                    _check_alive()
                    for observer in ("Loadc", "Loadh"):
                        snap = latest(observer, spore_t)
                        for m in _party_members(snap):
                            if m.get("name") == "Loadp":
                                for a in m.get("auras", []):
                                    if a.get("name") == "Gas Spore" and a.get("harmful"):
                                        return observer
                    return None

                debuff_observer = _poll(
                    _other_sees_nontank_debuff,
                    12,
                    "non-tank Loadp Gas Spore debuff visible to other raid members",
                )

                _poll(
                    lambda: any(
                        t >= spore_t
                        and c.get("bot") == "Loadp"
                        and (c.get("act") or {}).get("verb") == "TO"
                        and (c.get("act") or {}).get("thing") == "GROUP"
                        for t, c in list(proxy.commands)
                    ),
                    12,
                    "Loadp firing `Gas Spore on me -> to group` standing order",
                )
                server.console(f"llmtest unaura Loadp {GAS_SPORE_SPELL}", wait_s=0.2, inst=TEST)

                with proxy.lock:
                    vis_errs = list(proxy.visibility_errors)
            finally:
                for who in (HUMAN,) + BOTS:
                    server.console(f"llmtest ungroup {who}", wait_s=0.05, inst=TEST)
                    server.console(f"llmtest tele {who} {NORTHSHIRE}", wait_s=0.05, inst=TEST)
                server.console("llmtest clearstrat", wait_s=0.2, inst=TEST)
                if client is not None:
                    client.close()
                harness_proc.terminate()
                try:
                    harness_proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    harness_proc.kill()
                brain.stop()
                proxy.stop()

        expect(not vis_errs, f"visibility errors: {vis_errs}")
        return (
            f"25-man ICC raid: +icc preserved, 8k budget ok (max_est={max_est_tokens}), "
            f"shared role compiles={len(phase1_compiles)}/24, /rw spread+stack (2 tanks excluded), "
            f"Gunship add targeted by {len(sorc_bots)} bots, Festergut Gas Spore visible to "
            f"{debuff_observer} + TO GROUP fired"
        )

    return types.SimpleNamespace(
        NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=ONLINE, run=run
    )


CASES = [_case_orders(), _case_boss(), _case_spell_miss(), _case_raid_25man()]
