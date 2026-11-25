"""Grouped e2e cases: playability, dungeon_nav."""

import types
from wlblib import wire
from wlblib.testing import expect, find_option, send_cmd

def _case_playability():
    """Live verification of playability fixes: party chat, incremental talents, dead release.

    Verifies on the isolated `playability` test stack:
      1. High 4: When Loada and Telda are grouped and Loada sends a PARTY chat line,
         Telda hears it with `at_me=False` (bot-to-bot party chat must not trigger
         wake/reply loops).
      2. High 1 + R8614690-H4: When Loada levels to 15 (6 points) and then levels
         1 point at a time (16, then 17), `Spend talent points in <Tree>` spends all
         free points each time and clears `self.indicators.talent_points_unspent`.
      3. High 3 + Medium 1 + R8614690-H2: When Loada is killed (`llmtest kill Loada`),
         Loada's snapshot enters `DEAD_BODY`, offers `Release spirit`, choosing it
         succeeds without `"you died"` error, and Loada transitions to `GHOST`.
    """

    import socket
    import sys
    import time

    from wlblib import server
    from wlblib.common import ROOT, TEST
    from wlblib.testing import expect


    NAME = "playability"
    DESCRIPTION = "Party chat non-loop, incremental 1-pt talent spending, and dead Release spirit"
    TEST_CONFIG = "playability"
    BOT = "Loada"
    ONLINE = 2




    def run(stack):
        server.console("llmtest group Loada Telda", inst=TEST)
        server.console("character level Loada 15", inst=TEST)

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()

        cmd_id = 0
        party_sent = False
        heard_party_event = None
        # Talent stages: 0 = spend L15 (6 pts), 1 = spend L16 (1 pt), 2 = spend L17 (1 pt), 3 = done.
        talent_stage = 0
        talent_cmd_in_flight = False
        kill_issued = False
        release_cmd_sent = False
        ghost_verified = False
        leave_sent = False
        left_group = False
        errors = []

        deadline = time.time() + 45.0
        try:
            while time.time() < deadline and not (
                heard_party_event is not None
                and talent_stage == 3
                and ghost_verified
                and left_group
            ):
                try:
                    chunk = sock.recv(1 << 20)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                for payload in reader.feed(chunk):
                    msg = wire.decode_module_message(payload)
                    if not msg:
                        continue
                    if "event" in msg:
                        ev_bot = msg["event"].get("bot", "")
                        ev = msg["event"].get("event", {})
                        if "error" in ev:
                            errors.append(f"{ev_bot}: {ev['error'].get('text')}")
                        if "chat" in ev and ev_bot == "Telda":
                            ch = ev["chat"]
                            if (
                                ch.get("channel") == "PARTY"
                                and ch.get("from") == "Loada"
                                and ch.get("text") == "ready to pull"
                            ):
                                heard_party_event = ch
                        continue

                    snap = msg.get("snapshot")
                    if not snap:
                        continue
                    bot = snap.get("bot", "")
                    if bot != "Loada":
                        continue
                    seq = snap.get("seq", 0)
                    options = snap.get("options", [])
                    self_info = snap.get("base", {}).get("self", {})
                    indicators = self_info.get("indicators", {})
                    party_members = [
                        m.get("name", "")
                        for m in snap.get("ext", {}).get("party", {}).get("members", [])
                    ]

                    # Telda listed is not Telda in the world: a party line sent before she
                    # logs in reaches nobody (the full e2e run's intermittent failure).
                    telda_online = any(
                        m.get("name") == "Telda" and m.get("group", {}).get("online")
                        for m in snap.get("ext", {}).get("party", {}).get("members", [])
                    )
                    if not party_sent and telda_online:
                        cmd_id += 1
                        send_cmd(
                            sock,
                            {
                                "bot": "Loada",
                                "command_id": cmd_id,
                                "say": {"channel": "PARTY", "to": "", "text": "ready to pull"},
                            },
                        )
                        party_sent = True

                    # Incremental talent spending across L15 -> L16 (+1 pt) -> L17 (+1 pt).
                    if talent_stage < 3:
                        talent_idx = next(
                            (
                                i
                                for i, o in enumerate(options, 1)
                                if o.get("guid") == "talent:0"
                            ),
                            None,
                        )
                        unspent_flag = bool(indicators.get("talent_points_unspent", False))
                        if not talent_cmd_in_flight:
                            if talent_idx is not None:
                                cmd_id += 1
                                send_cmd(
                                    sock,
                                    {
                                        "bot": "Loada",
                                        "command_id": cmd_id,
                                        "choose": {"option": talent_idx, "seq": seq},
                                    },
                                )
                                talent_cmd_in_flight = True
                        elif talent_idx is None and not unspent_flag:
                            talent_cmd_in_flight = False
                            talent_stage += 1
                            if talent_stage == 1:
                                server.console("character level Loada 16", inst=TEST)
                            elif talent_stage == 2:
                                server.console("character level Loada 17", inst=TEST)

                    # Once talents + party chat are verified, move both into Ragefire
                    # Chasm with Telda knowing Resurrection (2006) so BrainPolicyListener
                    # holds auto-release, kill Loada, and verify Release spirit -> GHOST.
                    if (
                        talent_stage == 3
                        and heard_party_event is not None
                        and not ghost_verified
                    ):
                        state = self_info.get("state", "")
                        if not kill_issued:
                            server.console("llmtest learn Telda 2006", inst=TEST)
                            server.console("llmtest tele Loada 389 2.0 -8.0 -15.0 0", inst=TEST)
                            server.console("llmtest tele Telda 389 2.0 -8.0 -15.0 0", inst=TEST)
                            time.sleep(1.5)
                            server.console("llmtest kill Loada", inst=TEST)
                            kill_issued = True
                        elif state == "DEAD_BODY" and not release_cmd_sent:
                            rel_idx = next(
                                (
                                    i
                                    for i, o in enumerate(options, 1)
                                    if o.get("guid") == "rez:release"
                                ),
                                None,
                            )
                            if rel_idx is not None:
                                cmd_id += 1
                                send_cmd(
                                    sock,
                                    {
                                        "bot": "Loada",
                                        "command_id": cmd_id,
                                        "choose": {"option": rel_idx, "seq": seq},
                                    },
                                )
                                release_cmd_sent = True
                        elif release_cmd_sent and state == "GHOST":
                            ghost_verified = True

                    if ghost_verified and not left_group:
                        if "Telda" not in party_members:
                            left_group = True
                        elif not leave_sent:
                            leave_idx = next(
                                (
                                    i
                                    for i, o in enumerate(options, 1)
                                    if o.get("kind") == "LEAVE_GROUP"
                                ),
                                None,
                            )
                            if leave_idx is not None:
                                cmd_id += 1
                                send_cmd(
                                    sock,
                                    {
                                        "bot": "Loada",
                                        "command_id": cmd_id,
                                        "choose": {"option": leave_idx, "seq": seq},
                                    },
                                )
                                leave_sent = True
        finally:
            sock.close()

        expect(not errors, f"unexpected module errors: {errors}")
        expect(
            heard_party_event is not None,
            "Telda never received Loada's PARTY chat event",
        )
        expect(
            not heard_party_event.get("at_me", False),
            f"bot-to-bot PARTY chat must have at_me=False, got {heard_party_event}",
        )
        expect(
            talent_stage == 3,
            f"incremental talent spending stalled at stage {talent_stage} (expected 3)",
        )
        expect(
            release_cmd_sent and ghost_verified,
            f"dead Release spirit failed (release_cmd_sent={release_cmd_sent}, ghost_verified={ghost_verified})",
        )
        return "PARTY at_me=False; L15->L16->L17 incremental talents spent; DEAD_BODY->Release spirit->GHOST ok"
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_dungeon_nav():
    """Live dungeon navigation across stair-heavy and seam-prone Classic/TBC/WotLK entrances.

    Verifies that multi-room dungeon clustering (`ClusterDungeonHostiles`) and the
    navmesh seam-bridge (`FindDungeonSeamHop` + `TickLongWalk`) expose concrete
    `Explore` options at disconnected/steep entrance stairs and actually advance
    the bot down the stairs without returning `"stuck"`.
    """

    import math
    import time

    from wlblib import server
    from wlblib.common import TEST
    from wlblib.testing import expect

    NAME = "dungeon_nav"
    DESCRIPTION = "Explore advances across stair-heavy Classic/TBC/WotLK dungeon entrances"
    TEST_CONFIG = "dungeon_nav"
    BOT = "Loada"
    MOVED_AT_LEAST_YD = 10.0

    # Verified `areatrigger_teleport` entrance coordinates for stair-heavy /
    # long-corridor / seam-prone dungeons across Classic, TBC, and WotLK.
    DUNGEONS = [
        ("Shadowfang Keep", 33, -229.13, 2109.18, 76.89),
        ("Magisters' Terrace", 585, 7.09, -0.45, -2.80),
        ("Utgarde Keep", 574, 153.79, -86.55, 12.55),
        ("The Nexus", 576, 145.87, -10.55, -16.64),
        ("Azjol-Nerub", 601, 413.31, 795.97, 831.35),
        ("Gundrak", 604, 1891.84, 832.17, 176.67),
        ("Ahn'kahet", 619, 333.35, -1109.94, 69.77),
    ]


    def run(stack):
        server.console(f"llmtest gear {BOT} 80", inst=TEST)
        time.sleep(1.5)

        summaries = []
        for name, map_id, x, y, z in DUNGEONS:
            server.console(f"llmtest tele {BOT} {map_id} {x} {y} {z} 0", inst=TEST)
            time.sleep(2.0)

            t = stack.play(["Explore"], seconds=25, tail_s=10)
            expect(not t["closed"], f"{name}: the module closed the connection")
            expect(
                not t["never_offered"],
                f"{name}: Explore never offered (options seen: "
                f"{sorted({o for s in t['snapshots'] for o in s['options']})})",
            )
            (explore,) = t["commands"]
            # Reaching a hostile pack naturally interrupts a long walk with
            # "walk interrupted: you are in combat"; any other error (e.g. "stuck")
            # is a navigation failure.
            failed = [
                e["event"]["error"]
                for e in t["events"]
                if e["event"].get("error", {}).get("command_id") == explore["command_id"]
                and e["event"].get("error", {}).get("text", "")
                != "walk interrupted: you are in combat"
            ]
            expect(not failed, f"{name}: Explore '{explore['text']}' failed: {failed}")

            before = next(s for s in t["snapshots"] if s["seq"] == explore["seq"])
            after = [s for s in t["snapshots"] if s["seq"] >= explore["seq"]]
            max_moved = max(
                (math.dist(before["pos"], s["pos"]) for s in after),
                default=0.0,
            )
            expect(
                max_moved >= MOVED_AT_LEAST_YD,
                f"{name}: moved only {max_moved:.1f} yd after '{explore['text']}' "
                f"(start={before['pos']}, end={after[-1]['pos']})",
            )
            summaries.append(f"{name}={max_moved:.0f}yd")

        return ", ".join(summaries)
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

CASES = [_case_playability(), _case_dungeon_nav()]
