"""Live-model E2E team-play cases (Milestone 6a/6b): dungeon_clear, human_disconnect, llm_stall,
raid_boss.

Runs the real C++ `out/common/harness run` against the real local `llama-server` (:8090) and
isolated test worldserver (`wlb test live [NAME...]`). Kept separate from `wlb test e2e` so
mock-LLM e2e stays CPU-only, deterministic, shardable with `--remote`, and non-conflicting with
`eval`'s llama-server during `wlb test all`.
"""

import os
import pathlib
import re
import select
import socket
import subprocess
import tempfile
import threading
import time
import types

from wlblib import build, server, wowclient
from wlblib.common import OUT, ROOT, TEST, say
from wlblib.testing import LIVE_TEMPERATURE_ENV, ProxyRecorder, expect

NORTHSHIRE = "0 -8949.95 -132.49 83.53 0"
DESERTER = 71041


def _temperature():
    try:
        return float(os.environ.get(LIVE_TEMPERATURE_ENV, "0.0"))
    except ValueError:
        return 0.0


def _poll(pred, timeout_s, what, interval=0.5):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        val = pred()
        if val:
            return val
        time.sleep(interval)
    expect(False, f"timed out ({timeout_s} s) waiting for {what}")


def _launch_harness(proxy_sock, bots_dir, *, port=8090, slots=8, only_bots=None, notes_dir=None):
    cmd = [
        str(OUT / "common" / "harness"),
        "run",
        f"--socket={proxy_sock}",
        f"--rules={ROOT / 'content' / 'RULES.md'}",
        f"--bots-dir={bots_dir}",
        "--host=127.0.0.1",
        f"--port={port}",
        f"--slots={slots}",
        f"--temperature={_temperature():g}",
    ]
    if only_bots:
        cmd.append(f"--only-bots={','.join(only_bots)}")
    if notes_dir:
        cmd.append(f"--notes-dir={notes_dir}")
    proc = subprocess.Popen(
        cmd,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    log_lines = []

    def _drain():
        if proc.stdout is not None:
            for line in proc.stdout:
                log_lines.append(line)

    threading.Thread(target=_drain, daemon=True).start()
    return proc, log_lines


def _stop_harness(proc):
    if proc is None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def _reset_char(who):
    server.console("lfg clean", wait_s=0.2, inst=TEST)
    server.console(f"llmtest ungroup {who}", wait_s=0.2, inst=TEST)
    server.console(f"llmtest tele {who} {NORTHSHIRE}", wait_s=0.5, inst=TEST)
    server.console(f"llmtest unaura {who} {DESERTER}", wait_s=0.2, inst=TEST)


def _ensure_human_account(account, password):
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


class _LlmStallProxy:
    """Transparent TCP/HTTP proxy in front of real `llama-server` (:8090) that can toggle
    between forwarding real live-model traffic (`pass`) and simulating an outage (`fail`:
    HTTP 503 + dropping in-flight connections)."""

    def __init__(self, upstream_host="127.0.0.1", upstream_port=8090):
        self.upstream_host = upstream_host
        self.upstream_port = upstream_port
        self.lock = threading.Lock()
        self.mode = "pass"
        self.pass_connections = 0
        self.fail_responses = 0
        self._stop = threading.Event()
        self._active = []  # active client/upstream sockets to drop on outage
        self._srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._srv.bind(("127.0.0.1", 0))
        self.port = self._srv.getsockname()[1]
        self._srv.listen(32)
        self._srv.settimeout(0.2)
        self._thread = threading.Thread(target=self._accept_loop, daemon=True)

    def start(self):
        self._thread.start()

    def set_mode(self, mode):
        with self.lock:
            self.mode = mode
            to_close = list(self._active) if mode != "pass" else []
        for s in to_close:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                s.close()
            except OSError:
                pass

    def stop(self):
        self._stop.set()
        self.set_mode("fail")
        try:
            self._srv.close()
        except OSError:
            pass
        self._thread.join(timeout=5)

    def _accept_loop(self):
        while not self._stop.is_set():
            try:
                cli, _ = self._srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._handle, args=(cli,), daemon=True).start()

    def _handle(self, cli):
        with self.lock:
            mode = self.mode
        if mode != "pass":
            try:
                cli.settimeout(1.0)
                try:
                    cli.recv(4096)
                except OSError:
                    pass
                body = b'{"error":{"message":"simulated LLM outage"}}'
                resp = (
                    b"HTTP/1.1 503 Service Unavailable\r\n"
                    b"Content-Type: application/json\r\n"
                    + f"Content-Length: {len(body)}\r\n".encode("ascii")
                    + b"Connection: close\r\n\r\n"
                    + body
                )
                cli.sendall(resp)
                with self.lock:
                    self.fail_responses += 1
            except OSError:
                pass
            finally:
                try:
                    cli.close()
                except OSError:
                    pass
            return

        up = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            up.connect((self.upstream_host, self.upstream_port))
        except OSError:
            cli.close()
            up.close()
            return
        with self.lock:
            self.pass_connections += 1
            self._active.extend((cli, up))
        try:
            while not self._stop.is_set():
                with self.lock:
                    if self.mode != "pass":
                        break
                r, _, _ = select.select([cli, up], [], [], 0.2)
                if cli in r:
                    data = cli.recv(65536)
                    if not data:
                        break
                    up.sendall(data)
                if up in r:
                    data = up.recv(65536)
                    if not data:
                        break
                    cli.sendall(data)
        except OSError:
            pass
        finally:
            with self.lock:
                for s in (cli, up):
                    if s in self._active:
                        self._active.remove(s)
            for s in (cli, up):
                try:
                    s.close()
                except OSError:
                    pass


def _case_dungeon_clear():
    """Live-model 4-bot dungeon pulls + wipe + ghost corpse run & portal re-entry.

      1. Gear Loada (Prot Paladin tank), Loadb (Holy Priest healer), Loadc (Frost Mage DPS),
         and Loadd (Combat Rogue DPS) to level 70, group them (Loada leads & tanks), and
         place them inside Utgarde Keep (map 574).
      2. Run the real C++ harness against the real local llama-server:
         - Verify pull gate: out-of-combat non-tanks (Loadb/Loadc/Loadd) are never offered
           `Attack ... (not pulled)` options.
         - Verify real pulls & combat: the live LLM tank initiates combat inside Utgarde Keep
           and the party engages real enemies.
      3. Wipe the party (`llmtest kill` all 4 bots). Without any scripted release command, the
         live LLM chooses `Release spirit`, `BotManager::KeepCorpseRun` walks all 4 ghosts from
         the exterior Northrend graveyard to Utgarde Keep portal AreaTrigger 4745 (smooth walk,
         no cheat teleport), and all 4 bots re-enter Utgarde Keep and revive `ALIVE` with no
         Resurrection Sickness.
    """
    import math

    NAME = "dungeon_clear"
    DESCRIPTION = "Live LLM: Utgarde Keep pulls (pull gate + combat) and wipe -> ghost corpse run"
    TEST_CONFIG = "humandungeon"
    BOT = "Loada"
    ONLINE = 4

    BOTS = ("Loada", "Loadb", "Loadc", "Loadd")
    NON_TANKS = ("Loadb", "Loadc", "Loadd")
    # Prot Paladin, Discipline Priest (no Spirit of Redemption delay on wipe), Frost Mage, Combat Rogue.
    SPECS = {"Loada": 1, "Loadb": 0, "Loadc": 2, "Loadd": 1}
    UK_MAP = 574
    UK_ENTRANCE = "574 153.79 -86.55 12.55 0"
    UK_PORTAL_TRIGGER = 4745
    UK_PORTAL_POS = (1257.1, -4854.5, 41.2)
    MAX_GHOST_STEP_YD = 80.0

    def _where(snap):
        return snap.get("base", {}).get("where", {})

    def _state(snap):
        return snap.get("base", {}).get("self", {}).get("state", "")

    def _in_combat(snap):
        return bool(snap.get("base", {}).get("self", {}).get("in_combat", False))

    def _in_uk(snap):
        w = _where(snap)
        return bool(w.get("in_instance")) and "Utgarde Keep" in w.get("map", "")

    def _pos3(snap):
        p = _where(snap).get("position")
        return (p["x"], p["y"], p["z"]) if p else None

    def _has_rez_sickness(snap):
        debuffs = snap.get("base", {}).get("self", {}).get("debuffs", [])
        return any("Resurrection Sickness" in (d.get("name") or "") for d in debuffs)

    def run(stack):
        build.common("default", tests=False)
        log_start = len(stack.server_log())
        for bot in BOTS:
            server.console(f"llmtest gear {bot} 70 {SPECS[bot]}", wait_s=0.5, inst=TEST)
        server.console("llmtest maketank Loada", wait_s=0.5, inst=TEST)
        for member in NON_TANKS:
            server.console(f"llmtest group Loada {member}", wait_s=0.3, inst=TEST)
        time.sleep(1.5)
        for bot in BOTS:
            server.console(f"llmtest tele {bot} {UK_ENTRANCE}", wait_s=0.5, inst=TEST)
        time.sleep(2.5)

        with tempfile.TemporaryDirectory(prefix="wlb-live-dm-") as tmp:
            tmp_dir = pathlib.Path(tmp)
            bots_dir = tmp_dir / "bots"
            proxy_sock = tmp_dir / "proxy.sock"
            proxy = ProxyRecorder(stack.socket, proxy_sock)
            proxy.start()
            harness, harness_log = _launch_harness(
                proxy_sock,
                bots_dir,
                slots=8,
                notes_dir=ROOT / "content" / "notes",
            )

            def latest(bot, after=0.0):
                with proxy.lock:
                    return next(
                        (s for t, s in reversed(proxy.snapshots)
                         if s.get("bot") == bot and t >= after),
                        None,
                    )

            try:
                # 1. All 4 bots inside Utgarde Keep and making live LLM decisions.
                _poll(
                    lambda: all(
                        (s := latest(b)) is not None and _in_uk(s) and _state(s) == "ALIVE"
                        for b in BOTS
                    ),
                    30,
                    "all 4 bots alive inside Utgarde Keep",
                )
                _poll(
                    lambda: harness.poll() is None
                    and all(
                        any(c.get("bot") == b and "choose" in c for _, c in proxy.commands)
                        for b in BOTS
                    ),
                    60,
                    "live LLM decisions for all 4 bots",
                )

                # 2. Verify pull gate on non-tanks and wait for real pulls + combat.
                def _combat_engaged():
                    expect(harness.poll() is None,
                           f"harness exited early:\n{''.join(harness_log[-30:])}")
                    with proxy.lock:
                        combat_bots = {
                            s.get("bot")
                            for _, s in proxy.snapshots
                            if s.get("bot") in BOTS and _in_combat(s)
                        }
                    return "Loada" in combat_bots and len(combat_bots) >= 2

                _poll(_combat_engaged, 120, "tank Loada pulling and party entering combat in UK")

                # Check pull gate: before any bot entered combat, non-tanks must never have
                # been offered an unpulled Attack option.
                with proxy.lock:
                    first_combat_t = min(
                        (t for t, s in proxy.snapshots
                         if s.get("bot") in BOTS and _in_combat(s)),
                        default=0.0,
                    )
                    nontank_unpulled = [
                        (s.get("bot"), o.get("text"))
                        for t, s in proxy.snapshots
                        if t < first_combat_t
                        and s.get("bot") in NON_TANKS
                        and _in_uk(s)
                        for o in s.get("options", [])
                        if o.get("text", "").startswith("Attack ")
                        and o.get("text", "").endswith("(not pulled)")
                    ]
                    combat_bots = sorted({
                        s.get("bot")
                        for _, s in proxy.snapshots
                        if s.get("bot") in BOTS and _in_combat(s)
                    })

                expect(
                    not nontank_unpulled,
                    f"pull gate leaked unpulled Attack to non-tank before pull: "
                    f"{nontank_unpulled[:5]}",
                )
                say(f"live dungeon_clear: pull confirmed (combat_bots={combat_bots}); wiping party")

                # 3. Wipe all 4 bots; live LLM must Release spirit and KeepCorpseRun must
                #    walk all 4 ghosts from exterior graveyard -> portal 4745 -> revive ALIVE.
                wipe_t = time.monotonic()
                for bot in BOTS:
                    server.console(f"llmtest kill {bot}", wait_s=0.2, inst=TEST)

                # Wait for live LLM to release spirit on all 4 bots (no scripted command).
                # Note: a fast bot may transition DEAD_BODY -> GHOST before the last kill lands.
                _poll(
                    lambda: harness.poll() is None
                    and all(
                        any(
                            s.get("bot") == b
                            and t >= wipe_t
                            and _state(s) == "GHOST"
                            and not _where(s).get("in_instance")
                            for t, s in proxy.snapshots
                        )
                        for b in BOTS
                    ),
                    90,
                    "live LLM releasing spirit for all 4 wiped bots",
                )

                # Wait for all 4 bots to walk back to AreaTrigger 4745, re-enter UK, and revive.
                revived_snaps = {}

                def _all_revived():
                    expect(harness.poll() is None,
                           f"harness exited early during corpse run:\n{''.join(harness_log[-30:])}")
                    with proxy.lock:
                        for b in BOTS:
                            if b in revived_snaps:
                                continue
                            saw_ghost = False
                            for t, s in proxy.snapshots:
                                if t < wipe_t or s.get("bot") != b:
                                    continue
                                if _state(s) == "GHOST" and not _where(s).get("in_instance"):
                                    saw_ghost = True
                                elif saw_ghost and _in_uk(s) and _state(s) == "ALIVE":
                                    revived_snaps[b] = s
                                    break
                    return len(revived_snaps) == len(BOTS)

                _poll(_all_revived, 160, "all 4 bots reviving ALIVE back inside Utgarde Keep")

                with proxy.lock:
                    vis_errs = list(proxy.visibility_errors)
                    ghost_track = {b: [] for b in BOTS}
                    for t, s in proxy.snapshots:
                        if t < wipe_t:
                            continue
                        b = s.get("bot", "")
                        if b not in BOTS:
                            continue
                        if _state(s) == "GHOST" and not _where(s).get("in_instance"):
                            p = _pos3(s)
                            if p:
                                tr = ghost_track[b]
                                if not tr or math.dist(tr[-1][1:], p) >= 0.5:
                                    tr.append((t, *p))
                    final_snaps = dict(revived_snaps)
            finally:
                _stop_harness(harness)
                proxy.stop()
                for bot in BOTS:
                    server.console(f"llmtest ungroup {bot}", wait_s=0.2, inst=TEST)
                server.console("llmtest clearstrat", wait_s=0.2, inst=TEST)
                for bot in BOTS:
                    _reset_char(bot)

        expect(not vis_errs, f"visibility errors: {vis_errs}")
        log_text = stack.server_log()[log_start:]
        summaries = []
        for b in BOTS:
            track = ghost_track[b]
            expect(len(track) >= 3, f"{b}: expected >= 3 exterior ghost points, got {len(track)}")
            dists = [math.dist(pt[1:], UK_PORTAL_POS) for pt in track]
            start_d, min_d = dists[0], min(dists)
            max_step = max(
                (math.dist(track[i - 1][1:], track[i][1:]) for i in range(1, len(track))),
                default=0.0,
            )
            expect(start_d >= 100.0, f"{b}: ghost start_dist={start_d:.1f} yd (< 100)")
            expect(min_d <= 45.0, f"{b}: ghost min_dist={min_d:.1f} yd (> 45)")
            expect(max_step <= MAX_GHOST_STEP_YD, f"{b}: ghost jumped {max_step:.1f} yd")
            expect(not _has_rez_sickness(final_snaps[b]), f"{b}: revived with Resurrection Sickness")
            portal_line = (
                f"mod-llm-brain: {b} enters instance portal trigger "
                f"{UK_PORTAL_TRIGGER} for corpse on map {UK_MAP}"
            )
            expect(portal_line in log_text, f"{b}: missing portal re-entry log line")
            summaries.append(f"{b}({start_d:.0f}->{min_d:.0f}yd)")

        return (
            f"pull gate clean + combat ({','.join(combat_bots)}); "
            f"live LLM wipe release -> ghost walk -> portal {UK_PORTAL_TRIGGER} revive: "
            + ", ".join(summaries)
        )

    return types.SimpleNamespace(
        NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=ONLINE, run=run
    )


def _case_human_disconnect():
    """Live-model human disconnect inside an instance: bots wait instead of leaving/teleporting.

      1. Scripted human client (Allytwo, level 18 Warrior) invites the 4 Alliance bots; the
         live LLM accepts every invite and Allytwo becomes their master.
      2. All 5 characters enter Deadmines (map 36).
      3. Allytwo abruptly disconnects (`client.close()`). For 20 s with the live harness and
         real llama-server running, verify all 4 bots stay inside Deadmines and remain in the
         party (never leaving group, disbanding, or teleporting out).
      4. Allytwo reconnects and re-enters the world: all 4 bots are still grouped with Allytwo
         inside Deadmines.
    """
    NAME = "human_disconnect"
    DESCRIPTION = "Live LLM: human disconnects inside Deadmines; bots wait instead of leaving"
    TEST_CONFIG = "humandungeon"
    BOT = "Loada"
    ONLINE = 4

    BOTS = ("Loada", "Loadb", "Loadc", "Loadd")
    SPECS = {"Loada": 2, "Loadb": 1, "Loadc": 2, "Loadd": 1}
    HUMAN = "Allytwo"
    ACCOUNT = "HUMANDM"
    DM_ENTRANCE = "36 -16.4 -383.07 61.78 0"
    OFFLINE_HOLD_S = 20.0

    def _in_dm(snap):
        where = (snap or {}).get("base", {}).get("where", {})
        return bool(where.get("in_instance")) and "Deadmines" in where.get("map", "")

    def _party_names(snap):
        return [
            m.get("name", "")
            for m in ((snap or {}).get("ext") or {}).get("party", {}).get("members", [])
        ]

    def run(stack):
        build.common("default", tests=False)
        key = _ensure_human_account(ACCOUNT, "humandmpw")
        for bot in BOTS:
            server.console(f"llmtest gear {bot} 18 {SPECS[bot]}", wait_s=0.5, inst=TEST)

        with tempfile.TemporaryDirectory(prefix="wlb-live-dc-") as tmp:
            tmp_dir = pathlib.Path(tmp)
            proxy_sock = tmp_dir / "proxy.sock"
            proxy = ProxyRecorder(stack.socket, proxy_sock)
            proxy.start()
            harness, harness_log = _launch_harness(proxy_sock, tmp_dir / "bots", slots=8)
            client = None
            client2 = None

            def latest(bot, after=0.0):
                with proxy.lock:
                    return next(
                        (s for t, s in reversed(proxy.snapshots)
                         if s.get("bot") == bot and t >= after),
                        None,
                    )

            try:
                _poll(
                    lambda: all(latest(b) is not None for b in BOTS),
                    30,
                    "initial snapshots for all 4 bots",
                )
                client = wowclient.WowClient(TEST.world_port, ACCOUNT, key)
                client.authenticate()
                client.enter_world(HUMAN, race=1, cls=1)
                _reset_char(HUMAN)
                server.console(f"character level {HUMAN} 18", wait_s=0.5, inst=TEST)
                time.sleep(1.5)

                # 1. Human invites all 4 bots; live LLM accepts each invite.
                for bot in BOTS:
                    client.invite(bot)

                    def _is_master(bot=bot):
                        out = server.console(f"llmtest master {bot}", wait_s=0.5, inst=TEST)
                        m = re.search(rf"master {bot}=(\w+|-)", out)
                        return m is not None and m.group(1) == HUMAN

                    _poll(_is_master, 45, f"live LLM {bot} accepting invite from {HUMAN}")

                # 2. Teleport human + 4 bots into Deadmines.
                since_dm = time.monotonic()
                for who in (HUMAN,) + BOTS:
                    server.console(f"llmtest tele {who} {DM_ENTRANCE}", wait_s=0.5, inst=TEST)
                _poll(
                    lambda: all(
                        (s := latest(b, since_dm)) is not None
                        and _in_dm(s)
                        and HUMAN in _party_names(s)
                        for b in BOTS
                    ),
                    30,
                    "all 4 bots inside Deadmines with human in party",
                )

                # 3. Abrupt human disconnect inside instance: close client socket and kick
                #    the server session so Allytwo goes offline immediately (while remaining
                #    an offline member slot in the party), exercising the 30-min instance
                #    offline grace period out of combat.
                client.close()
                client = None
                server.console(f"kick {HUMAN}", wait_s=0.5, inst=TEST)
                _poll(
                    lambda: HUMAN not in server.online_characters(TEST),
                    20,
                    f"{HUMAN} going offline after disconnect",
                )

                # Hold for OFFLINE_HOLD_S while live harness + live LLM tick.
                hold_start = time.monotonic()
                while time.monotonic() - hold_start < OFFLINE_HOLD_S:
                    expect(
                        harness.poll() is None,
                        f"harness exited during human disconnect:\n{''.join(harness_log[-30:])}",
                    )
                    for b in BOTS:
                        s = latest(b, hold_start - 2.0)
                        expect(s is not None and _in_dm(s),
                               f"{b} left Deadmines after human disconnect: {s and s.get('base', {}).get('where')}")
                        members = _party_names(s)
                        expect(len(members) >= 3,
                               f"{b} left the party after human disconnect (members={members})")
                    time.sleep(1.0)

                # 4. Human reconnects; verify bots are still in Deadmines and in the party.
                key2 = os.urandom(40)
                wowclient.prepare_account(TEST.db_prefix, ACCOUNT, key2)
                client2 = wowclient.WowClient(TEST.world_port, ACCOUNT, key2)
                client2.authenticate()
                client2.enter_world(HUMAN, race=1, cls=1)
                reconn_t = time.monotonic()
                _poll(
                    lambda: HUMAN in server.online_characters(TEST)
                    and all(
                        (s := latest(b, reconn_t)) is not None
                        and _in_dm(s)
                        and HUMAN in _party_names(s)
                        for b in BOTS
                    ),
                    30,
                    f"all 4 bots still in Deadmines party after {HUMAN} reconnected",
                )
                with proxy.lock:
                    vis_errs = list(proxy.visibility_errors)
            finally:
                for who in (HUMAN,) + BOTS:
                    _reset_char(who)
                for c in (client, client2):
                    if c is not None:
                        c.close()
                _stop_harness(harness)
                proxy.stop()

        expect(not vis_errs, f"visibility errors: {vis_errs}")
        return (
            f"4 bots waited {OFFLINE_HOLD_S:.0f} s inside Deadmines after {HUMAN} disconnected "
            f"(stayed in instance + party) and resumed seamlessly on reconnect"
        )

    return types.SimpleNamespace(
        NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=ONLINE, run=run
    )


def _case_llm_stall():
    """Live-model LLM outage/stall: popup fallback to playerbots and automatic recovery.

      1. Group Loada and Loadb (level 20) and run the real harness through a switchable
         TCP/HTTP proxy to the real local llama-server (:8090). Verify both bots complete
         real live-LLM decisions (`pass` mode).
      2. Switch the proxy to `fail` mode (HTTP 503 outage) and wake Loadb:
         - Verify harness emits `Command::Health{llm_healthy=false}` and server logs
           `LLM unhealthy; handing popups to playerbots`.
         - Trigger a ready check and a group loot roll while the LLM is down; verify
           playerbots resolves the popups automatically (no brain `Choose` command needed).
      3. Switch the proxy back to `pass` mode and wake Loadb:
         - Verify live LLM succeeds, harness emits `Command::Health{llm_healthy=true}`, and
           server logs `LLM healthy; restoring brain popup ownership`.
         - Trigger a new ready check and verify the live LLM regains ownership and sends a
           brain `Choose` command to answer it.
    """
    NAME = "llm_stall"
    DESCRIPTION = "Live LLM outage -> playerbots popup fallback -> LLM recovery restores brain"
    TEST_CONFIG = "humandungeon"
    BOT = "Loada"
    ONLINE = 4

    ITEM = 1355  # Buckskin Cape (green)

    def run(stack):
        build.common("default", tests=False)
        log_start = len(stack.server_log())
        for bot in ("Loada", "Loadb"):
            server.console(f"character level {bot} 20", wait_s=0.3, inst=TEST)
        server.console("llmtest group Loada Loadb", wait_s=0.5, inst=TEST)

        with tempfile.TemporaryDirectory(prefix="wlb-live-stall-") as tmp:
            tmp_dir = pathlib.Path(tmp)
            proxy_sock = tmp_dir / "proxy.sock"
            proxy = ProxyRecorder(stack.socket, proxy_sock)
            llm_proxy = _LlmStallProxy("127.0.0.1", 8090)
            proxy.start()
            llm_proxy.start()
            harness, harness_log = _launch_harness(
                proxy_sock,
                tmp_dir / "bots",
                port=llm_proxy.port,
                slots=4,
                only_bots=("Loada", "Loadb"),
            )

            def _health_cmds(after=0.0):
                with proxy.lock:
                    return [
                        (t, bool((c.get("health") or {}).get("llm_healthy", False)))
                        for t, c in proxy.commands
                        if t >= after and "health" in c
                    ]

            def _loadb_chooses(after=0.0):
                with proxy.lock:
                    return [
                        (t, c)
                        for t, c in proxy.commands
                        if t >= after and c.get("bot") == "Loadb" and "choose" in c
                    ]

            def _loadb_latest(after=0.0):
                with proxy.lock:
                    return next(
                        (s for t, s in reversed(proxy.snapshots)
                         if s.get("bot") == "Loadb" and t >= after),
                        None,
                    )

            def _has_option(snap, prefix):
                return any(
                    o.get("text", "").startswith(prefix)
                    for o in (snap or {}).get("options", [])
                )

            try:
                # 1. Healthy live LLM baseline.
                _poll(
                    lambda: harness.poll() is None
                    and any(c.get("bot") == "Loada" and "choose" in c for _, c in proxy.commands)
                    and any(c.get("bot") == "Loadb" and "choose" in c for _, c in proxy.commands),
                    45,
                    "initial live LLM decisions for Loada and Loadb",
                )
                with llm_proxy.lock:
                    pass_before = llm_proxy.pass_connections
                expect(pass_before >= 1, "harness did not connect through LLM proxy")

                # 2. Simulate LLM outage -> health=false -> playerbots popup fallback.
                outage_t = time.monotonic()
                llm_proxy.set_mode("fail")
                # Trigger >= 2 wakes so BotLoop hits llm_unhealthy_after_failures (2).
                for i in range(3):
                    server.console(f"llmbrain whisper Loada Loadb ping{i}", wait_s=0.4, inst=TEST)
                    if any(h is False for _, h in _health_cmds(outage_t)):
                        break
                    time.sleep(1.0)

                _poll(
                    lambda: any(h is False for _, h in _health_cmds(outage_t))
                    and "mod-llm-brain: LLM unhealthy; handing popups to playerbots"
                    in stack.server_log()[log_start:],
                    30,
                    "LLM unhealthy health command and server fallback log",
                )

                # While LLM is down, trigger a group loot roll: playerbots (`LootRollAction`)
                # must resolve it automatically without any LLM call succeeding or brain Choose
                # answering the roll.
                with llm_proxy.lock:
                    pass_at_outage = llm_proxy.pass_connections
                popup_t = time.monotonic()
                out = server.console(f"llmtest lootroll Loada {ITEM}", wait_s=0.5, inst=TEST)
                expect("roll started" in out, f"setup: no loot roll started ({out.strip()[-200:]})")

                # Wait until Loadb has NO pending loot-roll options left (playerbots
                # auto-rolled well under the 60 s group loot timer).
                time.sleep(3.0)
                _poll(
                    lambda: (s := _loadb_latest(popup_t + 2.0)) is not None
                    and not _has_option(s, "Greed Buckskin Cape")
                    and not _has_option(s, "Need Buckskin Cape")
                    and not _has_option(s, "Pass Buckskin Cape"),
                    25,
                    "playerbots resolving loot roll while LLM is down",
                )
                with llm_proxy.lock:
                    pass_during_outage = llm_proxy.pass_connections - pass_at_outage
                expect(
                    pass_during_outage == 0,
                    f"LLM proxy forwarded {pass_during_outage} connections during outage",
                )
                # Pre-outage multi-step plans may still finish non-popup steps, but no
                # brain Choose may answer the ready check or loot roll during the outage.
                with proxy.lock:
                    snaps_by_seq = {
                        s.get("seq"): s.get("options", [])
                        for _, s in proxy.snapshots
                        if s.get("bot") == "Loadb"
                    }
                popup_prefixes = (
                    "Ready (confirm ready check)",
                    "Not ready",
                    "Need ",
                    "Greed ",
                    "Pass ",
                )
                outage_popup_chooses = []
                for _, cmd in _loadb_chooses(popup_t):
                    ch = cmd.get("choose") or {}
                    opts = snaps_by_seq.get(ch.get("seq"), [])
                    idx = int(ch.get("option", 0)) - 1
                    text = opts[idx].get("text", "") if 0 <= idx < len(opts) else ""
                    if text.startswith(popup_prefixes):
                        outage_popup_chooses.append((cmd, text))
                expect(
                    not outage_popup_chooses,
                    f"brain answered popup during LLM outage: {outage_popup_chooses}",
                )

                # 3. Restore live LLM -> health=true -> brain regains popup ownership.
                recover_t = time.monotonic()
                log_before_recover = len(stack.server_log())
                llm_proxy.set_mode("pass")
                server.console("llmbrain whisper Loada Loadb back online now", wait_s=0.5, inst=TEST)

                _poll(
                    lambda: any(h is True for _, h in _health_cmds(recover_t))
                    and "mod-llm-brain: LLM healthy; restoring brain popup ownership"
                    in stack.server_log()[log_before_recover:]
                    and "mod-llm-brain: Loadb is brain-led"
                    in stack.server_log()[log_before_recover:],
                    45,
                    "LLM healthy recovery command and server restore log",
                )

                # Verify live LLM is actively deciding again after recovery.
                post_rc_t = time.monotonic()
                server.console("llmbrain whisper Loada Loadb ready for next pull?", wait_s=0.5, inst=TEST)
                _poll(
                    lambda: len(_loadb_chooses(post_rc_t)) >= 1,
                    30,
                    "live LLM deciding again after recovery",
                )
                with proxy.lock:
                    vis_errs = list(proxy.visibility_errors)
                with llm_proxy.lock:
                    fails = llm_proxy.fail_responses
            finally:
                _stop_harness(harness)
                llm_proxy.stop()
                proxy.stop()
                server.console("lfg clean", wait_s=0.2, inst=TEST)
                server.console("llmtest ungroup Loadb", wait_s=0.2, inst=TEST)
                for bot in ("Loada", "Loadb"):
                    _reset_char(bot)

        expect(not vis_errs, f"visibility errors: {vis_errs}")
        return (
            f"outage ({fails}x 503) -> LLM unhealthy -> playerbots resolved loot roll "
            "(0 popup chooses) -> recovery -> brain-led restored & live LLM decided"
        )

    return types.SimpleNamespace(
        NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=ONLINE, run=run
    )


def _case_raid_boss():
    """Live-model 10-man raid boss encounter (9 bots + 1 human) in Vault of Archavon.

      1. Raid formation: scripted human client (Allyraid, level 80) invites 4 Alliance bots,
         converts to a raid (`CMSG_GROUP_RAID_CONVERT`), and invites 5 more Alliance bots
         (9 bots + 1 human = 10-man raid: Prot Paladin tank, Disc Priest + Resto Shaman
         healers, 6 DPS). Live LLM accepts every invite; verify all 9 bots see `ext.raid`
         with 9 other raid members.
      2. Pull discipline: teleport the 10-man raid into Vault of Archavon (map 624) outside
         Archavon the Stone Watcher's aggro radius. Verify non-tanks are never offered
         `Attack ... (not pulled)` and no bot prematurely pulls the boss.
      3. `/rw` tactical calls: human issues `/rw spread` and `/rw stack` (`CHAT_MSG_RAID_WARNING`).
         Verify all 9 bots apply both leader commands and physically spread out / stack back.
      4. Cooldowns / boost + clean boss engagement: verify `boost` combat strategy is active
         on grouped raid bots; tank pulls Archavon cleanly; raid engages and deals damage to
         Archavon; human calls `/rw lust` and the Draenei Shaman casts Heroism across the raid.
      5. Mid-fight `/rw stop dps` and `/rw resume dps`: human calls both via `/rw`; verify all
         9 bots apply both commands cleanly during the boss encounter.
    """
    import math

    NAME = "raid_boss"
    DESCRIPTION = "Live LLM: 10-man VoA raid boss (formation, /rw calls, boost/lust, pull gate)"
    TEST_CONFIG = "humanraid"
    BOT = "Loada"
    ONLINE = 9

    BOTS = ("Loada", "Loadb", "Loadc", "Loadd", "Loade", "Loadf", "Loadg", "Loadh", "Loadi")
    NON_TANKS = BOTS[1:]
    # Prot Paladin, Disc Priest, Resto Shaman (Heroism), Frost Mage, Combat Rogue,
    # MM Hunter, Affliction Warlock, Fury Warrior, Balance Druid.
    SPECS = {
        "Loada": 1, "Loadb": 0, "Loadc": 2, "Loadd": 2, "Loade": 1,
        "Loadf": 1, "Loadg": 0, "Loadh": 1, "Loadi": 0,
    }
    HUMAN = "Allyraid"
    ACCOUNT = "HUMANRAID"
    # Archavon the Stone Watcher sits at (138.9, -101.5, 91.4) in Vault of Archavon (map 624).
    # Stage at (100.0, -101.5, 91.4) (~39 yd west, clear LoS, outside aggro range, zero trash).
    STAGE_XY = (100.0, -101.5)
    VOA_STAGE = "624 100.0 -101.5 91.4 0.0"
    TANK_PULL_POS = "624 126.0 -101.5 91.4 0.0"
    BOSS_NAME = "Archavon the Stone Watcher"

    def _where(snap):
        return (snap or {}).get("base", {}).get("where", {})

    def _state(snap):
        return (snap or {}).get("base", {}).get("self", {}).get("state", "")

    def _in_combat(snap):
        return bool((snap or {}).get("base", {}).get("self", {}).get("in_combat", False))

    def _in_voa(snap):
        w = _where(snap)
        return bool(w.get("in_instance")) and "Archavon" in w.get("map", "")

    def _pos2(snap):
        p = _where(snap).get("position")
        return (p["x"], p["y"]) if p else None

    def _party_names(snap):
        return [
            m.get("name", "")
            for m in ((snap or {}).get("ext") or {}).get("party", {}).get("members", [])
        ]

    def _has_lust_aura(snap):
        auras = (snap or {}).get("base", {}).get("self", {}).get("unit", {}).get("auras", [])
        return any(
            (a.get("name") or "") in ("Heroism", "Bloodlust", "Exhaustion", "Sated")
            for a in auras
        )

    def _boss_hp_pct(snap):
        for u in (snap or {}).get("base", {}).get("nearby", {}).get("units", []):
            if BOSS_NAME in (u.get("name") or ""):
                return u.get("health_pct")
        tgt = ((snap or {}).get("ext") or {}).get("target", {}).get("target") or {}
        if BOSS_NAME in (tgt.get("name") or ""):
            return tgt.get("health_pct")
        return None

    def run(stack):
        build.common("default", tests=False)
        key = _ensure_human_account(ACCOUNT, "humanraidpw")
        for bot in BOTS:
            server.console(f"llmtest gear {bot} 80 {SPECS[bot]}", wait_s=0.5, inst=TEST)
        server.console("llmtest maketank Loada", wait_s=0.3, inst=TEST)

        with tempfile.TemporaryDirectory(prefix="wlb-live-raid-") as tmp:
            tmp_dir = pathlib.Path(tmp)
            proxy_sock = tmp_dir / "proxy.sock"
            proxy = ProxyRecorder(stack.socket, proxy_sock)
            proxy.start()
            harness, harness_log = _launch_harness(
                proxy_sock,
                tmp_dir / "bots",
                slots=8,
                notes_dir=ROOT / "content" / "notes",
            )
            client = None

            def latest(bot, after=0.0):
                with proxy.lock:
                    return next(
                        (s for t, s in reversed(proxy.snapshots)
                         if s.get("bot") == bot and t >= after),
                        None,
                    )

            def _check_alive():
                expect(
                    harness.poll() is None,
                    f"harness exited early:\n{''.join(harness_log[-30:])}",
                )

            try:
                _poll(
                    lambda: all(latest(b) is not None for b in BOTS),
                    40,
                    "initial snapshots for all 9 raid bots",
                )
                client = wowclient.WowClient(TEST.world_port, ACCOUNT, key)
                client.authenticate()
                client.enter_world(HUMAN, race=1, cls=1)
                _reset_char(HUMAN)
                server.console(f"character level {HUMAN} 80", wait_s=0.5, inst=TEST)
                time.sleep(1.0)

                # 1. Raid formation: invite 4 -> convert to raid -> invite remaining 5.
                def _invite_and_wait(bot):
                    # Clear any spontaneous bot-to-bot party formed while waiting for invites.
                    server.console(f"llmtest ungroup {bot}", wait_s=0.2, inst=TEST)
                    client.invite(bot)

                    def _is_master():
                        _check_alive()
                        out = server.console(f"llmtest master {bot}", wait_s=0.4, inst=TEST)
                        m = re.search(rf"master {bot}=(\w+|-)", out)
                        return m is not None and m.group(1) == HUMAN

                    _poll(_is_master, 45, f"live LLM {bot} accepting raid invite from {HUMAN}")

                for bot in BOTS[:4]:
                    _invite_and_wait(bot)
                client.convert_to_raid()
                time.sleep(1.0)
                for bot in BOTS[4:]:
                    _invite_and_wait(bot)

                formed_t = time.monotonic()
                _poll(
                    lambda: all(
                        (s := latest(b, formed_t)) is not None
                        and "raid" in (s.get("ext") or {})
                        and HUMAN in _party_names(s)
                        and len(_party_names(s)) == 9
                        for b in BOTS
                    ),
                    30,
                    "all 9 bots reporting 10-man raid membership (ext.raid + 9 others)",
                )
                say("live raid_boss: 10-man raid formed (9 bots + 1 human)")

                # Verify boost combat strategy is preserved on grouped raid bots.
                for check_bot in ("Loadc", "Loadd", "Loade"):
                    strat_out = server.console(
                        f"llmtest checkstrat {check_bot} boost", wait_s=0.3, inst=TEST
                    )
                    expect(
                        "combat=1" in strat_out,
                        f"expected boost combat strategy enabled on {check_bot}: {strat_out.strip()}",
                    )

                # 2. Teleport all 10 into Vault of Archavon and verify pull discipline.
                voa_t = time.monotonic()
                for who in (HUMAN,) + BOTS:
                    server.console(f"llmtest tele {who} {VOA_STAGE}", wait_s=0.4, inst=TEST)
                _poll(
                    lambda: all(
                        (s := latest(b, voa_t)) is not None
                        and _in_voa(s)
                        and _state(s) == "ALIVE"
                        for b in BOTS
                    ),
                    35,
                    "all 9 bots alive inside Vault of Archavon",
                )

                # Hold 4 s out of combat: verify no premature pull and no unpulled Attack leak.
                prepull_start = time.monotonic()
                time.sleep(4.0)
                prepull_end = time.monotonic()
                with proxy.lock:
                    nontank_unpulled = [
                        (s.get("bot"), o.get("text"))
                        for t, s in proxy.snapshots
                        if prepull_start <= t <= prepull_end
                        and s.get("bot") in NON_TANKS
                        and _in_voa(s)
                        for o in s.get("options", [])
                        if o.get("text", "").startswith("Attack ")
                        and o.get("text", "").endswith("(not pulled)")
                    ]
                    early_combat = sorted({
                        s.get("bot")
                        for t, s in proxy.snapshots
                        if prepull_start <= t <= prepull_end
                        and s.get("bot") in BOTS
                        and _in_combat(s)
                    })
                expect(
                    not nontank_unpulled,
                    f"pull gate leaked unpulled Attack to non-tank: {nontank_unpulled[:5]}",
                )
                expect(
                    not early_combat,
                    f"bots prematurely pulled boss before raid pull: {early_combat}",
                )
                say("live raid_boss: pull discipline verified inside Vault of Archavon")

                # 3. /rw tactical calls: spread and stack.
                log_spread = len(stack.server_log())
                spread_t = time.monotonic()
                client.raid_warning("spread")
                _poll(
                    lambda: all(
                        f"mod-llm-brain: {b} applied leader command spread from {HUMAN}"
                        in stack.server_log()[log_spread:]
                        for b in BOTS
                    ),
                    20,
                    "all 9 bots applying /rw spread",
                )

                def _spread_out():
                    _check_alive()
                    moved = 0
                    for b in BOTS:
                        s = latest(b, spread_t)
                        p = _pos2(s)
                        if p and math.dist(p, STAGE_XY) >= 5.0:
                            moved += 1
                    return moved >= 6

                _poll(_spread_out, 20, ">= 6/9 bots physically spreading out >= 5 yd")

                log_stack = len(stack.server_log())
                stack_t = time.monotonic()
                client.raid_warning("stack")
                _poll(
                    lambda: all(
                        f"mod-llm-brain: {b} applied leader command stack from {HUMAN}"
                        in stack.server_log()[log_stack:]
                        for b in BOTS
                    ),
                    20,
                    "all 9 bots applying /rw stack",
                )

                def _stacked_up():
                    _check_alive()
                    near = 0
                    for b in BOTS:
                        s = latest(b, stack_t)
                        p = _pos2(s)
                        if p and math.dist(p, STAGE_XY) <= 6.5:
                            near += 1
                    return near >= 7

                _poll(_stacked_up, 25, ">= 7/9 bots stacking back within 6.5 yd of leader")
                say("live raid_boss: /rw spread and /rw stack verified")

                # 4. Clean boss pull + engagement + /rw lust (Heroism).
                pull_t = time.monotonic()
                server.console(f"llmtest tele Loada {TANK_PULL_POS}", wait_s=0.4, inst=TEST)
                client.raid_warning("pull")

                def _raid_engaged():
                    _check_alive()
                    with proxy.lock:
                        combat_bots = {
                            s.get("bot")
                            for t, s in proxy.snapshots
                            if t >= pull_t and s.get("bot") in BOTS and _in_combat(s)
                        }
                    return "Loada" in combat_bots and len(combat_bots) >= 7

                _poll(_raid_engaged, 45, "tank Loada + >= 7/9 raid bots engaging Archavon")

                def _heroism_active():
                    _check_alive()
                    lusted = sum(
                        1 for b in BOTS if _has_lust_aura(latest(b, pull_t))
                    )
                    return lusted >= 5

                log_lust = len(stack.server_log())
                lust_t = time.monotonic()
                if not _heroism_active():
                    client.raid_warning("lust")
                    # If Loadc was mid-cast (e.g. healing tank) when the first /rw lust landed,
                    # re-call once after 5 s; if boost already auto-cast Heroism, _heroism_active()
                    # passes immediately.
                    deadline = time.monotonic() + 25.0
                    retried = False
                    while time.monotonic() < deadline and not _heroism_active():
                        if not retried and time.monotonic() - lust_t >= 5.0:
                            client.raid_warning("lust")
                            retried = True
                        time.sleep(0.5)
                loadc_auras = [
                    a.get("name", "")
                    for a in (latest("Loadc", pull_t) or {})
                    .get("base", {})
                    .get("self", {})
                    .get("unit", {})
                    .get("auras", [])
                ]
                expect(
                    _heroism_active(),
                    f"Heroism/Exhaustion not active on >= 5/9 raid bots after /rw lust "
                    f"(Loadc auras={loadc_auras}, log_applied="
                    f"{f'mod-llm-brain: Loadc applied leader command lust from {HUMAN}' in stack.server_log()[log_lust:]})",
                )

                def _boss_damaged():
                    _check_alive()
                    with proxy.lock:
                        for t, s in reversed(proxy.snapshots):
                            if t < pull_t:
                                break
                            hp = _boss_hp_pct(s)
                            if hp is not None and hp < 100:
                                return hp
                    return None

                boss_hp = _poll(_boss_damaged, 30, "Archavon taking damage (< 100% HP)")
                say(f"live raid_boss: Archavon engaged (hp={boss_hp}%) + Heroism active")

                # 5. Mid-fight /rw stop dps -> /rw resume dps.
                log_stop = len(stack.server_log())
                client.raid_warning("stop dps")
                _poll(
                    lambda: all(
                        f"mod-llm-brain: {b} applied leader command stop dps from {HUMAN}"
                        in stack.server_log()[log_stop:]
                        for b in BOTS
                    ),
                    20,
                    "all 9 bots applying /rw stop dps",
                )
                time.sleep(2.0)

                log_resume = len(stack.server_log())
                client.raid_warning("resume dps")
                _poll(
                    lambda: all(
                        f"mod-llm-brain: {b} applied leader command resume dps from {HUMAN}"
                        in stack.server_log()[log_resume:]
                        for b in BOTS
                    ),
                    20,
                    "all 9 bots applying /rw resume dps",
                )

                lusted_bots = sum(1 for b in BOTS if _has_lust_aura(latest(b, pull_t)))
                with proxy.lock:
                    vis_errs = list(proxy.visibility_errors)
                    combat_bots = sorted({
                        s.get("bot")
                        for t, s in proxy.snapshots
                        if t >= pull_t and s.get("bot") in BOTS and _in_combat(s)
                    })
            finally:
                for who in (HUMAN,) + BOTS:
                    _reset_char(who)
                server.console("llmtest clearstrat", wait_s=0.2, inst=TEST)
                if client is not None:
                    client.close()
                _stop_harness(harness)
                proxy.stop()

        expect(not vis_errs, f"visibility errors: {vis_errs}")
        return (
            f"10-man raid formed (9 bots + {HUMAN}), pull gate clean, "
            f"/rw spread/stack/stop-dps/resume-dps applied by 9/9, "
            f"boost active + Heroism on {lusted_bots}/9, "
            f"Archavon engaged ({len(combat_bots)}/9 in combat, boss hp={boss_hp}%)"
        )

    return types.SimpleNamespace(
        NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=ONLINE, run=run
    )


CASES = [_case_dungeon_clear(), _case_human_disconnect(), _case_llm_stall(), _case_raid_boss()]
