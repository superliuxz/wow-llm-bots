"""Command reply speed (Companion-lessons gap 4): end-to-end latency from a
human leader's party command to the bot's LLM chat reply.

Runs `humanparty` (real scripted client Allyone + Loada, Loadb, Loadc), the
real C++ `harness run`, and the real `llama-server` (`LlamaServer`). After a
warm-up command to prime the KV cache, sends a sequence of addressed party
commands rotating across the 3 bots (so no bot hits the 3-wakes-per-10s
per-sender rate limit) and measures the wall-clock time from `CMSG_MESSAGECHAT`
to the bot's `SMSG_MESSAGECHAT` party reply received by the client.

Gate: p95 <= 2.0 s (`docs/TODO.md` Companion-lessons gap 4).
"""

from __future__ import annotations

import math
import os
import pathlib
import re
import subprocess
import tempfile
import time

from wlblib import build, server, wowclient
from wlblib.common import OUT, ROOT, TEST, say
from wlblib.testing import LlamaServer, ProxyRecorder, expect

NAME = "command_reply"
DESCRIPTION = "p95 latency from a human party command to the bot's LLM chat reply (<= 2 s)"
TEST_CONFIG = "humanparty"
BOT = "Loada"
ONLINE = 3

BOTS = ("Loada", "Loadb", "Loadc")
HUMAN = "Allyone"
ACCOUNT = "HUMANTEST"

REPLY_P95_LIMIT_S = 2.0
REPLY_TIMEOUT_S = 5.0
TARGET_REPLIES = 6
MIN_REPLIES = 3

# Distinct party calls/questions addressed to each bot that expect a brief LLM
# chat reply (avoiding bare "wait here"/"follow me", where RULES.md tells bots
# "reply briefly if at all"). Rotating across BOTS keeps each bot below the
# 3-wakes-per-10s per-sender rate limit.
TRIALS = (
    ("Loada", "loada are you ready to pull?"),
    ("Loadb", "loadb can you heal me?"),
    ("Loadc", "loadc are you ready to pull?"),
    ("Loada", "loada where should we go next?"),
    ("Loadb", "loadb are you ready to pull?"),
    ("Loadc", "loadc where should we go next?"),
    ("Loada", "loada anyone up for deadmines?"),
    ("Loadb", "loadb can you buff me please?"),
    ("Loadc", "loadc anyone up for deadmines?"),
    ("Loada", "loada ready for the next pull?"),
    ("Loadb", "loadb where should we go next?"),
    ("Loadc", "loadc ready for the next pull?"),
)


def percentile(values: list[float], pct: float) -> float:
    """Nearest-rank percentile for a non-empty list of floats (pct in [0, 100])."""
    if not values:
        raise ValueError("percentile requires at least one value")
    ordered = sorted(values)
    if pct <= 0:
        return ordered[0]
    if pct >= 100:
        return ordered[-1]
    idx = max(0, math.ceil((pct / 100.0) * len(ordered)) - 1)
    return ordered[min(idx, len(ordered) - 1)]


def _account_ready() -> bool:
    out = subprocess.run(
        ["mysql", f"--defaults-file={OUT / 'server' / 'etc' / 'mysql.cnf'}", "-uroot", "-N",
         "-e", f"select id from {TEST.db_prefix}auth.account where username = '{ACCOUNT}'"],
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    return bool(out)


def _poll(pred, timeout_s: float, what: str, step_s: float = 0.2):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        val = pred()
        if val:
            return val
        time.sleep(step_s)
    expect(False, f"timed out ({timeout_s} s) waiting for {what}")


def run(perf):
    build.common("default", tests=False)
    if not _account_ready():
        server.console(f"account create {ACCOUNT} humantestpw", wait_s=1.0, inst=TEST)
        _poll(_account_ready, 20, "the human's account")
    key = os.urandom(40)
    wowclient.prepare_account(TEST.db_prefix, ACCOUNT, key)

    with LlamaServer(slots=4), tempfile.TemporaryDirectory(prefix="wlb-perf-cmdreply-") as tmp:
        tmp_dir = pathlib.Path(tmp)
        proxy_sock = tmp_dir / "proxy.sock"
        proxy = ProxyRecorder(perf.stack.socket, proxy_sock)
        proxy.start()
        harness = subprocess.Popen(
            [str(OUT / "common" / "harness"), "run", f"--socket={proxy_sock}",
             f"--rules={ROOT / 'content' / 'RULES.md'}", f"--bots-dir={tmp_dir / 'bots'}",
             "--host=127.0.0.1", "--port=8090", "--slots=4", "--temperature=0"],
            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        client = None
        latencies_s: list[float] = []
        decision_s: list[float] = []
        replies_seen: list[tuple[str, float, str]] = []
        try:
            # Wait until all 3 bots have received initial snapshots and made their first decision.
            _poll(
                lambda: set(BOTS) <= {
                    c.get("bot") for _, c in list(proxy.commands) if c.get("bot")
                },
                60,
                "every bot's initial LLM decision",
                step_s=0.5,
            )

            client = wowclient.WowClient(TEST.world_port, ACCOUNT, key)
            client.authenticate()
            client.enter_world(HUMAN, race=1, cls=1)
            time.sleep(1.5)

            for bot in BOTS:
                client.invite(bot)

                def _is_master(bot=bot):
                    out = server.console(f"llmtest master {bot}", wait_s=0.4, inst=TEST)
                    m = re.search(rf"master {bot}=(\w+|-)", out)
                    return bool(m and m.group(1) == HUMAN)

                _poll(_is_master, 30, f"{bot} accepting invite with {HUMAN} as master", step_s=0.5)

            # Move the party away from Deputy Willem / Marshal McBride to a
            # quiet open spot so bots aren't constantly woken by starter-quest
            # turn-in walks during the latency trials.
            for who in (HUMAN, *BOTS):
                server.console(f"llmtest tele {who} 0 -8880 -132.5 82 0", wait_s=0.3, inst=TEST)
            time.sleep(2.0)

            # Warm up llama-server's shared prompt prefix KV cache with one
            # unmeasured party line (single-responder wakes 1 bot), then wait
            # briefly so no warmup reply bleeds into trial 1 and no bot is near
            # the 3-wakes-per-10s per-sender rate limit.
            t0 = time.monotonic()
            client.say(wowclient.CHAT_MSG_PARTY, "anyone ready?")
            _poll(
                lambda: any(t >= t0 for t, _ in list(proxy.commands)),
                15,
                "warm-up LLM decision",
                step_s=0.1,
            )
            time.sleep(2.0)

            for idx, (bot, text) in enumerate(TRIALS, 1):
                t_send = time.monotonic()
                client.say(wowclient.CHAT_MSG_PARTY, text)
                deadline = t_send + REPLY_TIMEOUT_S
                reply_msg = None
                dec_t = None
                while time.monotonic() < deadline:
                    if dec_t is None:
                        with proxy.lock:
                            cmd = next(
                                ((t, c) for t, c in proxy.commands
                                 if t >= t_send and c.get("bot") == bot),
                                None,
                            )
                        if cmd is not None:
                            dec_t = cmd[0]
                    with client.lock:
                        reply_msg = next(
                            (c for c in client.chats
                             if c["t"] >= t_send
                             and c["type"] in ("party", "whisper")
                             and c["from_guid"] != client.guid
                             and c["text"].strip().lower() not in ("", "none")),
                            None,
                        )
                    if reply_msg is not None:
                        break
                    time.sleep(0.02)
                if dec_t is not None:
                    decision_s.append(max(0.0, dec_t - t_send))
                if reply_msg is not None:
                    dt = max(0.0, reply_msg["t"] - t_send)
                    latencies_s.append(dt)
                    replies_seen.append((bot, dt, reply_msg["text"]))
                    say(f"  trial {idx}/{len(TRIALS)} ({bot}): {dt:.3f} s "
                        f"[{reply_msg['type']}] -> {reply_msg['text']!r}")
                    if len(latencies_s) >= TARGET_REPLIES:
                        break
                else:
                    dec_note = f" (decision {dec_t - t_send:.3f} s)" if dec_t is not None else ""
                    say(f"  trial {idx}/{len(TRIALS)} ({bot}): no chat reply within "
                        f"{REPLY_TIMEOUT_S:.0f} s{dec_note}")
                # Space trials >= 1.5 s apart so rotating across 3 bots keeps
                # each bot at <= 2 wakes per 10 s (below kMaxChatWakesPerSender).
                time.sleep(1.5)

            client.say(wowclient.CHAT_MSG_PARTY, "follow me")
        finally:
            if client:
                client.close()
            proxy.stop()
            harness.terminate()
            try:
                harness.wait(timeout=5)
            except subprocess.TimeoutExpired:
                harness.kill()
            for bot in BOTS:
                server.console(f"llmtest ungroup {bot}", wait_s=0.2, inst=TEST)

    expect(latencies_s, f"no bot replied in party across {len(TRIALS)} command trials")
    p50_s = percentile(latencies_s, 50)
    p95_s = percentile(latencies_s, 95)
    max_s = max(latencies_s)

    perf.measure("reply_p50_s", p50_s, " s", factor=1.5, slack=0.3)
    perf.measure("reply_p95_s", p95_s, " s", factor=1.5, slack=0.3, limit=REPLY_P95_LIMIT_S)
    perf.measure("reply_max_s", max_s, " s", factor=2.0, slack=0.5)
    perf.measure("replies", len(latencies_s), "", lower_is_better=False, factor=1.0,
                 limit=MIN_REPLIES)
    if decision_s:
        perf.measure("decision_p95_s", percentile(decision_s, 95), " s",
                     factor=1.5, slack=0.3, limit=REPLY_P95_LIMIT_S)
