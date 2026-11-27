"""What 4 Hz combat snapshots cost at raid scale: 25 level-80 bots in one raid fighting a
raid-boss-ranked Heroic Training Dummy + flanking Grandmaster's Training Dummies in Orgrimmar's
Valley of Honor, with realistic quest logs (5 quests each), one harness connected.

Measures both delivery (age when received, frames/s, in-combat ratio) and the module's own
world-thread build cost (once-a-minute stats lines in the server log).
"""

import collections
import re
import socket
import time

from wlblib import build, server, wire
from wlblib.common import OUT, TEST, say
from wlblib.testing import expect

NAME = "raid_snapshots"
DESCRIPTION = "4 Hz combat snapshot build cost and delivery, 25-player raid fighting boss + adds"
TEST_CONFIG = "raid_pressure"
BOT = "Loada"
ONLINE = 25

BOTS = tuple(f"Load{chr(ord('a') + i)}" for i in range(ONLINE))
# Orgrimmar Valley of Honor: Heroic Training Dummy (level 83 WORLD_BOSS) at (2015.18, -4842.3)
# flanked by Grandmaster's Training Dummies at (2003.31, -4839.54) and (2025.73, -4845.15).
# Stand in melee range (~2 yd) of the Heroic Training Dummy so melee auto-attacks keep all 25
# raid bots continuously in combat even after mana is spent.
ARENA_TELE = "1 2015 -4840 51.8 4.71"
SAMPLE_QUESTS = (1, 25, 55, 124, 152)

RECEIVE_S = 135
WARMUP_S = 15.0

BUILD = re.compile(r"snapshots in the last minute: n=(\d+) build p50=(\d+)us p99=(\d+)us "
                   r"max=(\d+)us total=(\d+)ms bytes avg=(\d+) max=(\d+)")
TICK = re.compile(r"one tick built at most (\d+) snapshots, in at most (\d+) us")
SENT = re.compile(r"sent n=(\d+) \(built to socket p50=(\d+)us p99=(\d+)us max=(\d+)us\), "
                  r"(\d+) replaced by a newer snapshot, (\d+) dropped")


def last_full_combat_minute(log):
    """The module's last stats lines covering a full minute of 25-bot 4 Hz raid combat
    (n >= 25 * 180, i.e. >= 3 Hz average across all 25 bots)."""
    min_n = ONLINE * 180
    full = [m for m in BUILD.finditer(log) if int(m.group(1)) >= min_n]
    expect(full, f"the module logged no full minute of raid combat snapshots (n >= {min_n}) in "
                 f"{RECEIVE_S} s: {[m.group(1) for m in BUILD.finditer(log)]}")
    b = full[-1]
    t = TICK.search(log, b.end())
    s = SENT.search(log, b.end())
    expect(t and s, "the build line has no tick and sent lines after it")
    return b, t, s


def _percentile(sorted_values: list[float], q: float) -> float:
    if not sorted_values:
        return 0.0
    return sorted_values[min(len(sorted_values) - 1, int(q * len(sorted_values)))]


def _setup_raid():
    """Groups all 25 bots into a raid, gives them realistic quest logs, and moves them to the
    boss + adds arena."""
    leader = BOTS[0]
    for bot in BOTS[1:]:
        server.console(f"llmtest group {leader} {bot}", wait_s=0.1, inst=TEST)
    for bot in BOTS:
        server.console(f"llmtest tele {bot} {ARENA_TELE}", wait_s=0.05, inst=TEST)
    for bot in BOTS:
        for qid in SAMPLE_QUESTS:
            server.console(f"llmtest questadd {bot} {qid}", wait_s=0.02, inst=TEST)
    time.sleep(2.0)


def _drive_and_measure(sock_path: str, seconds: float, warmup: float) -> dict:
    """Connects to the module socket, keeps all 25 raid bots attacking the Heroic Training Dummy,
    and measures steady-state snapshot delivery."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.settimeout(0.2)
    wire.send_frame(s, wire.encode_command(wire.hello_command("wlb perf raid_snapshots")))

    reader = wire.FrameReader()
    ages: list[float] = []
    arrivals: list[float] = []
    in_combat_flags: list[bool] = []
    seen_bots: set[str] = set()
    last_engage: dict[str, float] = {}
    cmd_id = 0
    visibility_errors: list[str] = []

    t_start = time.time()
    end = t_start + seconds
    try:
        while time.time() < end:
            try:
                chunk = s.recv(1 << 20)
            except socket.timeout:
                continue
            if not chunk:
                break
            received_ms = time.monotonic_ns() / 1e6
            now = time.time()
            for payload in reader.feed(chunk):
                message = wire.decode_module_message(payload)
                if not message or "snapshot" not in message:
                    continue
                snap = message["snapshot"]
                bot = snap.get("bot", "")
                seen_bots.add(bot)
                built_ms = snap.get("monotonic_us", 0) / 1000.0
                in_combat = bool(snap.get("base", {}).get("self", {}).get("in_combat", False))

                ages.append(received_ms - built_ms)
                arrivals.append(received_ms / 1000.0)
                in_combat_flags.append(in_combat)

                # Spot-check visibility and P0 combat facts on sampled combat snapshots.
                if in_combat and len(visibility_errors) < 5 and len(ages) % 200 == 0:
                    errs = wire.visibility_errors(payload)
                    if errs:
                        visibility_errors.extend(errs[:5])
                    ext = snap.get("ext", {})
                    if not ext.get("party", {}).get("members"):
                        visibility_errors.append("combat snapshot missing ext.party.members")
                    if not ext.get("raid"):
                        visibility_errors.append("combat snapshot missing ext.raid")
                    if not ext.get("action_bars", {}).get("buttons"):
                        visibility_errors.append("combat snapshot missing ext.action_bars.buttons")
                    if "combat" not in ext:
                        visibility_errors.append("combat snapshot missing ext.combat")

                # Keep every bot engaged on the raid boss dummy.
                if not in_combat and now - last_engage.get(bot, 0.0) >= 3.0:
                    options = snap.get("options", [])
                    pick = None
                    for idx, opt in enumerate(options, 1):
                        text = opt.get("text", "")
                        if text == "Attack Heroic Training Dummy":
                            pick = idx
                            break
                    if pick is None:
                        for idx, opt in enumerate(options, 1):
                            if "Training Dummy" in opt.get("text", "") and opt.get("text", "").startswith("Attack "):
                                pick = idx
                                break
                    if pick is not None:
                        cmd_id += 1
                        last_engage[bot] = now
                        wire.send_frame(s, wire.encode_command({
                            "bot": bot,
                            "command_id": cmd_id,
                            "choose": {"option": pick, "seq": snap.get("seq", 0)},
                        }))
    finally:
        s.close()

    if not ages:
        return {"error": "no snapshots received"}

    first = arrivals[0]
    steady_idx = [i for i, t in enumerate(arrivals) if t - first >= warmup]
    steady_s = max(1.0, (arrivals[-1] - (first + warmup)) if steady_idx else seconds)
    steady_ages = sorted(ages[i] for i in steady_idx)
    steady_combat = sum(1 for i in steady_idx if in_combat_flags[i])
    return {
        "bots": len(seen_bots),
        "snapshots": len(ages),
        "steady_snapshots": len(steady_idx),
        "steady_per_s": len(steady_idx) / steady_s,
        "in_combat_pct": (100.0 * steady_combat / len(steady_idx)) if steady_idx else 0.0,
        "age_p50_ms": _percentile(steady_ages, 0.50),
        "age_p95_ms": _percentile(steady_ages, 0.95),
        "age_max_ms": steady_ages[-1] if steady_ages else 0.0,
        "visibility_errors": visibility_errors,
    }


def run(perf):
    if not (OUT / "common" / "libwlb_wire_py.so").exists():
        build.common("default", tests=False)

    say(f"setting up {ONLINE}-player raid at Valley of Honor training dummies")
    _setup_raid()

    r = _drive_and_measure(str(perf.stack.socket), RECEIVE_S, WARMUP_S)
    expect("error" not in r, f"raid snapshot measurement failed: {r}")
    expect(r["bots"] == ONLINE, f"snapshots from {r['bots']} bots, expected {ONLINE}")
    expect(not r["visibility_errors"], f"snapshot visibility errors: {r['visibility_errors']}")

    perf.measure("in_combat_pct", r["in_combat_pct"], "%", lower_is_better=False,
                 factor=1.05, limit=85.0)
    perf.measure("frames_per_s", r["steady_per_s"], "/s", lower_is_better=False,
                 factor=1.1, limit=ONLINE * 3.0)
    perf.measure("age_p50_ms", r["age_p50_ms"], " ms", slack=0.5)
    perf.measure("age_p95_ms", r["age_p95_ms"], " ms", slack=1.0)
    perf.measure("age_max_ms", r["age_max_ms"], " ms", factor=3, slack=5.0)

    b, t, s = last_full_combat_minute(perf.stack.server_log())
    perf.measure("build_p50_us", int(b.group(2)), " us", slack=50)
    perf.measure("build_p99_us", int(b.group(3)), " us", slack=150, limit=500)
    perf.measure("build_max_us", int(b.group(4)), " us", factor=3, slack=500, limit=3500)
    perf.measure("build_total_ms_per_min", int(b.group(5)), " ms", slack=100)
    perf.measure("snapshot_bytes_avg", int(b.group(6)), " B", factor=1.3)
    perf.measure("tick_max_snapshots", int(t.group(1)), "", factor=2, slack=2)
    perf.measure("tick_max_us", int(t.group(2)), " us", factor=3, slack=500, limit=5000)
    perf.measure("built_to_socket_p99_us", int(s.group(3)), " us", slack=500)
    perf.measure("dropped", int(s.group(6)), "", factor=1, limit=0)
