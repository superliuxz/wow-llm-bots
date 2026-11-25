"""Grouped e2e cases: invite, group, tank, popups, rez_ready, lootroll, lfg_proposal."""

import types
from wlblib import wire
from wlblib.testing import expect, find_option, send_cmd

def _case_invite():
    """One brain bot invites another; Playerbots must NOT auto-accept before the brain can.

    Verifies High 2 (Review ede3f6c):
      1. Loada whispers Telda and chooses `Invite Telda`.
      2. Telda receives the pending group invite and holds it for >= 2 s (Playerbots'
         `default` strategy must NOT auto-accept it).
      3. Telda's snapshot offers `Accept group invite` and `Decline group invite`.
      4. When the harness chooses `Accept group invite` for Telda, it succeeds with
         no "no pending group invite" error and both bots are in the same party.
    """

    import socket
    import sys
    import time

    from wlblib import server
    from wlblib.common import ROOT, TEST, running_pid
    from wlblib.testing import expect


    NAME = "invite"
    DESCRIPTION = "Loada invites Telda; Telda holds the invite until the brain accepts"
    TEST_CONFIG = "whisper"  # config/tests/whisper.*.overrides: two level-1 priests.
    BOT = "Loada"
    ONLINE = 2




    def run(stack):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()

        cmd_id = 0
        whisper_sent = False
        invite_sent_at = None
        saw_pending_invite_at = None
        accept_sent = False
        grouped = False
        errors = []
        latest_snaps = {}

        deadline = time.time() + 30.0
        try:
            while time.time() < deadline and not grouped:
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
                        ev = msg["event"].get("event", {})
                        if "error" in ev:
                            errors.append(f"{msg['event'].get('bot')}: {ev['error'].get('text')}")
                        continue
                    snap = msg.get("snapshot")
                    if not snap:
                        continue
                    bot = snap.get("bot", "")
                    seq = snap.get("seq", 0)
                    latest_snaps[bot] = snap
                    options = snap.get("options", [])
                    party_members = [
                        m.get("name", "")
                        for m in snap.get("ext", {}).get("party", {}).get("members", [])
                    ]

                    if bot == "Telda" and not whisper_sent and "Loada" in latest_snaps:
                        cmd_id += 1
                        send_cmd(sock, {
                            "bot": "Telda",
                            "command_id": cmd_id,
                            "say": {"channel": "WHISPER", "to": "Loada", "text": "group up?"},
                        })
                        whisper_sent = True
                        continue

                    if bot == "Loada" and whisper_sent and invite_sent_at is None:
                        inv_idx = next(
                            (i for i, o in enumerate(options, 1)
                             if o.get("kind") == "INVITE" and o.get("name") == "Telda"),
                            None,
                        )
                        if inv_idx is not None:
                            cmd_id += 1
                            send_cmd(sock, {
                                "bot": "Loada",
                                "command_id": cmd_id,
                                "choose": {"option": inv_idx, "seq": seq},
                            })
                            invite_sent_at = time.time()
                        continue

                    if bot == "Telda" and invite_sent_at is not None and not accept_sent:
                        # Must NOT already be in a party before the brain accepts!
                        expect(
                            "Loada" not in party_members,
                            f"Playerbots auto-accepted Loada's invite before the brain could! party={party_members}",
                        )
                        acc_idx = next(
                            (i for i, o in enumerate(options, 1) if o.get("kind") == "ACCEPT_INVITE"),
                            None,
                        )
                        dec_idx = next(
                            (i for i, o in enumerate(options, 1) if o.get("kind") == "DECLINE_INVITE"),
                            None,
                        )
                        if acc_idx is not None and dec_idx is not None:
                            if saw_pending_invite_at is None:
                                saw_pending_invite_at = time.time()
                            # Hold for >= 2.0 s to prove Playerbots does not steal the invite.
                            if time.time() - saw_pending_invite_at >= 2.0:
                                cmd_id += 1
                                send_cmd(sock, {
                                    "bot": "Telda",
                                    "command_id": cmd_id,
                                    "choose": {"option": acc_idx, "seq": seq},
                                })
                                accept_sent = True
                        continue

                    if bot == "Telda" and accept_sent and "Loada" in party_members:
                        grouped = True
                        break
        finally:
            sock.close()

        expect(invite_sent_at is not None, "Loada never saw/sent Invite Telda option")
        expect(saw_pending_invite_at is not None, "Telda never saw pending Accept/Decline group invite options")
        expect(accept_sent, "Test never reached brain-driven Accept group invite")
        expect(not errors, f"Invite/Accept produced errors: {errors}")
        expect(grouped, "Telda did not join Loada's party after brain accepted invite")

        # Verify bot logout destroys the Engine + BlockAutoInviteListener cleanly without
        # crashing worldserver, and that BotManager logs Telda back in and delivers fresh
        # snapshots after relogin (regression test for static-listener double-free on logout).
        server.console("llmtest logout Telda", inst=TEST)
        # Wait for Telda to actually go offline in the characters table.
        offline_deadline = time.time() + 15.0
        saw_offline = False
        while time.time() < offline_deadline:
            expect(running_pid("worldserver", TEST) is not None,
                   "worldserver crashed after logging out Telda!")
            if "Telda" not in server.online_characters(TEST):
                saw_offline = True
                break
            time.sleep(0.25)
        expect(saw_offline, "Telda never went offline after `llmtest logout Telda`")
        # Wait for BotManager to log Telda back in.
        server.wait_online("Telda", timeout_s=30, inst=TEST)
        expect(running_pid("worldserver", TEST) is not None,
               "worldserver crashed after Telda logged back in!")
        # Reconnect and confirm Telda produces a fresh snapshot post-relogin.
        sock2 = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock2.settimeout(0.5)
        sock2.connect(str(TEST.run / "brain.sock"))
        reader2 = wire.FrameReader()
        saw_relogin_snap = False
        snap_deadline = time.time() + 15.0
        try:
            while time.time() < snap_deadline and not saw_relogin_snap:
                try:
                    chunk = sock2.recv(1 << 20)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                for payload in reader2.feed(chunk):
                    msg = wire.decode_module_message(payload)
                    if msg and msg.get("snapshot", {}).get("bot") == "Telda":
                        saw_relogin_snap = True
                        break
        finally:
            sock2.close()
        expect(saw_relogin_snap, "Telda did not emit a fresh snapshot after relogin")
        return "Telda held invite >= 2.0 s, joined Loada's party, and survived logout/relogin without crash"
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_group():
    """Grouping with another player: quest sharing, roles, wait/follow (RH-H3, M1, M2).

      1. Level Loada and Telda to 20, group them (Loada leads), give Loada a quest.
      2. Telda's party panel shows Loada's role (spec-derived, not only in LFG).
      3. Loada chooses `Share quest ...`; Telda gets a QUEST_SHARE prompt event and
         an `Accept shared quest ...` option; choosing it takes the quest, so
         Loada's share option goes away.
      4. Telda chooses `Wait here`; then `Follow the party leader again` is offered.
    """

    import socket
    import sys
    import time

    from wlblib import server
    from wlblib.common import ROOT, TEST
    from wlblib.testing import expect


    NAME = "group"
    DESCRIPTION = "Quest sharing, spec roles and wait/follow in a hand-formed party"
    TEST_CONFIG = "whisper"  # config/tests/whisper.*.overrides: two level-1 priests.
    BOT = "Loada"
    ONLINE = 2
    # The Coastal Menace: sharable, any race, level 15+, and not auto-accept (a
    # shared auto-accept quest lands in the log at once, with nothing to answer).
    QUEST_ID = 104






    def run(stack):
        for bot in ("Loada", "Telda"):
            server.console(f"character level {bot} 20", inst=TEST)
        server.console("llmtest group Loada Telda", inst=TEST)
        server.console(f"llmtest questadd Loada {QUEST_ID}", inst=TEST)

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()

        cmd_id = 0
        sent = {}  # step -> command_id
        role_seen = None
        share_prompt = False
        share_gone = False
        loada_overlap = False
        telda_overlap = False
        wait_done = False
        follow_offered = False
        errors = []

        def choose(bot, idx, seq, step):
            nonlocal cmd_id
            cmd_id += 1
            sent[step] = cmd_id
            send_cmd(sock, {"bot": bot, "command_id": cmd_id,
                             "choose": {"option": idx, "seq": seq}})

        deadline = time.time() + 45.0
        try:
            while time.time() < deadline and not (
                share_gone and follow_offered and loada_overlap and telda_overlap
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
                        bot = msg["event"].get("bot")
                        ev = msg["event"].get("event", {})
                        err = ev.get("error")
                        if err and err.get("command_id") in sent.values():
                            errors.append(f"{bot}: {err.get('text')}")
                        prompt = ev.get("prompt")
                        if (bot == "Telda" and prompt
                                and prompt.get("kind") in ("QUEST_SHARE", 9)):
                            share_prompt = True
                        continue
                    snap = msg.get("snapshot")
                    if not snap:
                        continue
                    bot, seq = snap.get("bot", ""), snap.get("seq", 0)
                    options = snap.get("options", [])
                    members = snap.get("ext", {}).get("party", {}).get("members", [])
                    tracker = snap.get("ext", {}).get("quest_tracker", [])
                    if bot == "Telda":
                        for m in members:
                            if m.get("name") == "Loada":
                                role_seen = m.get("group", {}).get("role") or role_seen
                        for obj in tracker:
                            q = obj.get("quest") or {}
                            if q.get("quest_id") == QUEST_ID and "Loada" in (q.get("party_members") or []):
                                telda_overlap = True
                        if share_prompt and "accept" not in sent:
                            idx = find_option(options, "Accept shared quest")
                            if idx:
                                choose("Telda", idx, seq, "accept")
                        elif "accept" in sent and "wait" not in sent:
                            idx = find_option(options, "Wait here")
                            if idx:
                                choose("Telda", idx, seq, "wait")
                        elif "wait" in sent and not follow_offered:
                            wait_done = True
                            follow_offered = find_option(
                                options, "Follow the party leader again") is not None
                    if bot == "Loada":
                        for obj in tracker:
                            q = obj.get("quest") or {}
                            if q.get("quest_id") == QUEST_ID and "Telda" in (q.get("party_members") or []):
                                loada_overlap = True
                        idx = find_option(options, "Share quest")
                        if "share" not in sent and idx and members:
                            choose("Loada", idx, seq, "share")
                        elif "accept" in sent and idx is None:
                            share_gone = True
        finally:
            sock.close()
            server.console("llmtest ungroup Telda", inst=TEST)  # Leave no group.

        expect(role_seen not in (None, "", "ROLE_UNSPECIFIED", 0),
               f"Telda's party panel showed no role for Loada (got {role_seen!r})")
        expect("share" in sent, "Loada was never offered `Share quest` while grouped")
        expect(share_prompt, "Telda got no QUEST_SHARE prompt event after Loada shared")
        expect("accept" in sent, "Telda was never offered `Accept shared quest`")
        expect(share_gone, "Loada's share option stayed after Telda took the quest")
        expect(loada_overlap and telda_overlap,
               f"quest overlap missing after share (loada={loada_overlap}, telda={telda_overlap})")
        expect(wait_done and follow_offered,
               "`Wait here` did not turn into `Follow the party leader again`")
        expect(not errors, f"group choices produced errors: {errors}")
        return (f"Loada's role shown as {role_seen}; quest shared, accepted, and overlap shown; "
                "wait/follow toggled")
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_tank():
    """Two tanks in a party: the brain lets the other tank, and can take it back (RH-M1).

      1. Level Loada and Telda to 20, group them, and give both playerbots' tank
         strategy (`+tank assist`, the strategy that makes a bot a tank).
      2. Telda is offered `Let Loada tank (stop tanking)`; choosing it removes
         Telda's `tank assist` (checked with `llmtest istank`), and offers
         `Tank for the group again`.
      3. Choosing that gives Telda `tank assist` back.
    """

    import re
    import socket
    import sys
    import time

    from wlblib import server
    from wlblib.common import ROOT, TEST
    from wlblib.testing import expect


    NAME = "tank"
    DESCRIPTION = "With two tanks grouped, the brain stands down as tank and back"
    TEST_CONFIG = "whisper"  # config/tests/whisper.*.overrides: two level-1 priests.
    BOT = "Loada"
    ONLINE = 2






    def _telda_tanks():
        out = server.console("llmtest istank Telda", inst=TEST)
        m = re.search(r"istank Telda=(\d)", out)
        return None if not m else m.group(1) == "1"


    def run(stack):
        for bot in ("Loada", "Telda"):
            server.console(f"character level {bot} 20", inst=TEST)
            server.console(f"llmtest maketank {bot}", inst=TEST)
        server.console("llmtest group Loada Telda", inst=TEST)
        tanked_before = _telda_tanks()

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()
        cmd_id = 0
        sent = {}
        errors = []
        stood_down = None
        tank_again = None

        def choose(idx, seq, step):
            nonlocal cmd_id
            cmd_id += 1
            sent[step] = cmd_id
            send_cmd(sock, {"bot": "Telda", "command_id": cmd_id,
                             "choose": {"option": idx, "seq": seq}})

        deadline = time.time() + 45.0
        try:
            while time.time() < deadline and tank_again is None:
                try:
                    chunk = sock.recv(1 << 20)
                except socket.timeout:
                    continue
                for payload in reader.feed(chunk):
                    msg = wire.decode_module_message(payload)
                    if not msg:
                        continue
                    if "event" in msg:
                        err = msg["event"].get("event", {}).get("error")
                        if err and err.get("command_id") in sent.values():
                            errors.append(err.get("text"))
                        continue
                    snap = msg.get("snapshot")
                    if not snap or snap.get("bot") != "Telda":
                        continue
                    seq, options = snap.get("seq", 0), snap.get("options", [])
                    if "notank" not in sent:
                        idx = find_option(options, "Let Loada tank")
                        if idx:
                            choose(idx, seq, "notank")
                            time.sleep(2.5)  # BotManager applies it on its 1 s tick.
                            stood_down = _telda_tanks() is False
                    elif "tank" not in sent:
                        idx = find_option(options, "Tank for the group again")
                        if idx:
                            choose(idx, seq, "tank")
                            time.sleep(2.5)
                            tank_again = _telda_tanks() is True
        finally:
            sock.close()
            server.console("llmtest ungroup Telda", inst=TEST)  # Leave no group.
            server.console("llmtest clearstrat", inst=TEST)

        expect(tanked_before, "setup: Telda did not have `tank assist`")
        expect("notank" in sent, "Telda was never offered `Let Loada tank (stop tanking)`")
        expect(stood_down, "Telda still had `tank assist` after letting Loada tank")
        expect("tank" in sent, "Telda was never offered `Tank for the group again`")
        expect(tank_again, "Telda did not get `tank assist` back")
        expect(not errors, f"choices produced errors: {errors}")
        return "Telda stood down as tank for Loada and took tanking back"
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_popups():
    """A timed group popup wakes the brain while it is still open (RH-H1).

    The leader queues the party for Dungeon Finder; the member must get a
    `ROLE_CHECK` prompt event at once (not only on its next unrelated wake) and a
    snapshot that offers `Confirm Dungeon Finder role`. Answering it must work.

      1. Level Loada and Telda to 20 and group them (Loada leads).
      2. Choose Loada's `Queue Random Dungeon` option.
      3. Telda gets an ImmediateEvent `prompt: ROLE_CHECK` within 5 s, then a
         snapshot with a `Confirm Dungeon Finder role` option.
      4. Choosing it succeeds without an error event.
    """

    import socket
    import sys
    import time

    from wlblib import server
    from wlblib.common import ROOT, TEST
    from wlblib.testing import expect


    NAME = "popups"
    DESCRIPTION = "LFG role check wakes the party member's brain while the popup is open"
    TEST_CONFIG = "whisper"  # config/tests/whisper.*.overrides: two level-1 priests.
    BOT = "Loada"
    ONLINE = 2




    def _is_role_check(prompt):
        return prompt.get("kind") in ("ROLE_CHECK", 6)


    def run(stack):
        for bot in ("Loada", "Telda"):
            server.console(f"character level {bot} 20", inst=TEST)
        server.console("llmtest group Loada Telda", inst=TEST)

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()

        cmd_id = 0
        queue_cmd = None
        queued_at = None
        role_prompt_at = None
        role_cmd = None
        role_sent_at = None
        role_done = False
        errors = []

        deadline = time.time() + 40.0
        try:
            while time.time() < deadline and not role_done:
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
                        bot = msg["event"].get("bot")
                        ev = msg["event"].get("event", {})
                        if "error" in ev:
                            err = ev["error"]
                            if err.get("command_id") in (queue_cmd, role_cmd):
                                errors.append(f"{bot}: {err.get('text')}")
                        prompt = ev.get("prompt")
                        if (bot == "Telda" and prompt and _is_role_check(prompt)
                                and queued_at is not None and role_prompt_at is None):
                            role_prompt_at = time.time()
                        continue
                    snap = msg.get("snapshot")
                    if not snap:
                        continue
                    bot = snap.get("bot", "")
                    seq = snap.get("seq", 0)
                    options = snap.get("options", [])
                    members = [m.get("name", "") for m in
                               snap.get("ext", {}).get("party", {}).get("members", [])]

                    if bot == "Loada" and queue_cmd is None and "Telda" in members:
                        idx = next((i for i, o in enumerate(options, 1)
                                    if o.get("text", "").startswith("Queue Random Dungeon")),
                                   None)
                        if idx is not None:
                            cmd_id += 1
                            queue_cmd = cmd_id
                            send_cmd(sock, {"bot": "Loada", "command_id": cmd_id,
                                             "choose": {"option": idx, "seq": seq}})
                            queued_at = time.time()
                        continue

                    if bot == "Telda" and role_prompt_at is not None and role_cmd is None:
                        idx = next((i for i, o in enumerate(options, 1)
                                    if o.get("text", "").startswith("Confirm Dungeon Finder role")),
                                   None)
                        if idx is not None:
                            cmd_id += 1
                            role_cmd = cmd_id
                            send_cmd(sock, {"bot": "Telda", "command_id": cmd_id,
                                             "choose": {"option": idx, "seq": seq}})
                            role_sent_at = time.time()
                        continue

                    if role_cmd is not None and time.time() - role_sent_at >= 2.0:
                        role_done = True
                        break
        finally:
            sock.close()
            server.console("lfg clean", inst=TEST)
            server.console("llmtest ungroup Telda", inst=TEST)  # Leave no group.

        expect(queue_cmd is not None, "Loada never offered `Queue Random Dungeon` while grouped")
        expect(role_prompt_at is not None,
               "Telda got no ROLE_CHECK prompt event after Loada queued (popup did not wake the brain)")
        delay = role_prompt_at - queued_at
        expect(delay <= 5.0, f"ROLE_CHECK prompt took {delay:.1f} s (> 5 s)")
        expect(role_cmd is not None, "Telda's snapshot never offered `Confirm Dungeon Finder role`")
        expect(not errors, f"queue/role choice produced errors: {errors}")
        return f"ROLE_CHECK woke Telda {delay:.2f} s after Loada queued; role confirmed without error"
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_rez_ready():
    """Ready check and resurrection popups wake the brain; waiting for a rez (RH-H1, RH-M3).

      1. Level Loada and Telda to 20 and group them (Loada leads).
      2. Loada starts a ready check: Telda gets a READY_CHECK prompt event and a
         `Ready (confirm ready check)` option; choosing it works.
      3. Telda dies; it chooses `Wait for a resurrection`, and is still lying
         dead (not released) 8 s later.
      4. Loada offers a resurrection: Telda gets a RESURRECT_OFFER prompt event and
         an `Accept resurrection` option; choosing it brings Telda back alive.
    """

    import socket
    import sys
    import time

    from wlblib import server
    from wlblib.common import ROOT, TEST
    from wlblib.testing import expect


    NAME = "rez_ready"
    DESCRIPTION = "Ready check and rez offer wake the brain; Wait for a resurrection holds the release"
    TEST_CONFIG = "whisper"  # config/tests/whisper.*.overrides: two level-1 priests.
    BOT = "Loada"
    ONLINE = 2
    STILL_DEAD_S = 8.0






    def run(stack):
        for bot in ("Loada", "Telda"):
            server.console(f"character level {bot} 20", inst=TEST)
        server.console("llmtest group Loada Telda", inst=TEST)

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()

        cmd_id = 0
        sent = {}
        prompts = set()
        errors = []
        phase = "ready"  # ready -> die -> wait -> rez -> done
        waited_at = None
        still_dead_ok = False
        alive_again = False

        def choose(idx, seq, step):
            nonlocal cmd_id
            cmd_id += 1
            sent[step] = cmd_id
            send_cmd(sock, {"bot": "Telda", "command_id": cmd_id,
                             "choose": {"option": idx, "seq": seq}})

        server.console("llmtest readycheck Loada", inst=TEST)
        deadline = time.time() + 60.0
        try:
            while time.time() < deadline and phase != "done":
                try:
                    chunk = sock.recv(1 << 20)
                except socket.timeout:
                    chunk = None
                for payload in reader.feed(chunk or b""):
                    msg = wire.decode_module_message(payload)
                    if not msg:
                        continue
                    if "event" in msg:
                        if msg["event"].get("bot") != "Telda":
                            continue
                        ev = msg["event"].get("event", {})
                        err = ev.get("error")
                        if err and err.get("command_id") in sent.values():
                            errors.append(err.get("text"))
                        if ev.get("prompt"):
                            prompts.add(str(ev["prompt"].get("kind")))
                        continue
                    snap = msg.get("snapshot")
                    if not snap or snap.get("bot") != "Telda":
                        continue
                    seq, options = snap.get("seq", 0), snap.get("options", [])
                    state = snap.get("base", {}).get("self", {}).get("state", "")
                    if phase == "ready" and "READY_CHECK" in prompts:
                        idx = find_option(options, "Ready (confirm ready check)")
                        if idx:
                            choose(idx, seq, "ready")
                            server.console("llmtest kill Telda", inst=TEST)
                            phase = "die"
                    elif phase == "die" and state == "DEAD_BODY":
                        idx = find_option(options, "Wait for a resurrection")
                        if idx:
                            choose(idx, seq, "wait")
                            waited_at = time.time()
                            phase = "wait"
                    elif phase == "wait":
                        if state != "DEAD_BODY":
                            break  # Released (or rezzed) despite waiting.
                        if time.time() - waited_at >= STILL_DEAD_S:
                            still_dead_ok = True
                            server.console("llmtest rezoffer Telda Loada", inst=TEST)
                            phase = "rez"
                    elif phase == "rez" and "RESURRECT_OFFER" in prompts and "accept" not in sent:
                        idx = find_option(options, "Accept resurrection")
                        if idx:
                            choose(idx, seq, "accept")
                    elif phase == "rez" and "accept" in sent and state == "ALIVE":
                        alive_again = True
                        phase = "done"
        finally:
            sock.close()
            server.console("llmtest ungroup Telda", inst=TEST)  # Leave no group.

        expect("READY_CHECK" in prompts, f"no READY_CHECK prompt for Telda (got {prompts})")
        expect("ready" in sent, "Telda was never offered `Ready (confirm ready check)`")
        expect("wait" in sent, "dead Telda was never offered `Wait for a resurrection`")
        expect(still_dead_ok, f"Telda did not stay lying dead {STILL_DEAD_S:.0f} s after choosing to wait")
        expect("RESURRECT_OFFER" in prompts, f"no RESURRECT_OFFER prompt for Telda (got {prompts})")
        expect(alive_again, "Telda was not alive after `Accept resurrection`")
        expect(not errors, f"choices produced errors: {errors}")
        return (f"ready check answered; waited {STILL_DEAD_S:.0f} s dead; "
                "rez offer woke Telda and accepting revived her")
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_lootroll():
    """A group loot roll wakes the brain while it is open (RH-H1).

      1. Level Loada and Telda to 20 and group them (Loada leads).
      2. `llmtest lootroll` kills a boar near Loada and starts a group roll on a
         green item (Buckskin Cape) under group loot.
      3. Telda gets a `loot_roll` event at once and `Need` / `Greed` / `Pass`
         options; Loada rolls Need, and Telda's options then say `[Need rolled by
         Loada]` (Now #7); choosing `Greed` is accepted (no error), and the roll
         options go away.
    """

    import socket
    import sys
    import time

    from wlblib import server
    from wlblib.common import ROOT, TEST
    from wlblib.testing import expect


    NAME = "lootroll"
    DESCRIPTION = "A group loot roll wakes the member's brain; Greed is accepted"
    TEST_CONFIG = "whisper"  # config/tests/whisper.*.overrides: two level-1 priests.
    BOT = "Loada"
    ONLINE = 2
    ITEM = 1355  # Buckskin Cape (green).




    def run(stack):
        for bot in ("Loada", "Telda"):
            server.console(f"character level {bot} 20", inst=TEST)
        server.console("llmtest group Loada Telda", inst=TEST)

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()
        out = server.console(f"llmtest lootroll Loada {ITEM}", inst=TEST)
        started_at = time.time()
        expect("roll started" in out, f"setup: no roll started ({out.strip()[-200:]})")

        roll_event_at = None
        loada_needed = False
        greed_cmd = None
        greed_sent_at = None
        greed_text = ""
        options_gone = False
        errors = []
        deadline = time.time() + 30.0
        try:
            while time.time() < deadline and not options_gone:
                try:
                    chunk = sock.recv(1 << 20)
                except socket.timeout:
                    continue
                for payload in reader.feed(chunk):
                    msg = wire.decode_module_message(payload)
                    if not msg:
                        continue
                    if "event" in msg:
                        if msg["event"].get("bot") != "Telda":
                            continue
                        ev = msg["event"].get("event", {})
                        if "loot_roll" in ev and roll_event_at is None:
                            roll_event_at = time.time()
                        err = ev.get("error")
                        if err and err.get("command_id") == greed_cmd:
                            errors.append(err.get("text"))
                        continue
                    snap = msg.get("snapshot")
                    if not snap:
                        continue
                    options = snap.get("options", [])
                    if snap.get("bot") == "Loada":
                        # Now #7: Loada needs first; Telda's options must say so.
                        need = next((i for i, o in enumerate(options, 1)
                                     if o.get("text", "").startswith("Need Buckskin Cape")), None)
                        if need and not loada_needed:
                            loada_needed = True
                            send_cmd(sock, {"bot": "Loada", "command_id": 98,
                                             "choose": {"option": need, "seq": snap.get("seq", 0)}})
                        continue
                    if snap.get("bot") != "Telda":
                        continue
                    greed = next((i for i, o in enumerate(options, 1)
                                  if o.get("text", "").startswith("Greed Buckskin Cape")
                                  and "Need rolled by Loada" in o.get("text", "")), None)
                    if greed_cmd is None and roll_event_at is not None and greed:
                        greed_text = options[greed - 1].get("text", "")
                        greed_cmd = 99
                        send_cmd(sock, {"bot": "Telda", "command_id": greed_cmd,
                                         "choose": {"option": greed, "seq": snap.get("seq", 0)}})
                        greed_sent_at = time.time()
                    elif greed_cmd is not None and time.time() - greed_sent_at > 1.0:
                        options_gone = not any("Buckskin Cape" in o.get("text", "")
                                               for o in options)
        finally:
            sock.close()
            server.console("llmtest ungroup Telda", inst=TEST)  # Leave no group.

        expect(roll_event_at is not None, "Telda got no loot_roll event after the roll started")
        delay = roll_event_at - started_at
        expect(delay <= 5.0, f"loot_roll event took {delay:.1f} s (> 5 s)")
        expect(greed_cmd is not None, "Telda was never offered `Greed Buckskin Cape`")
        # F2: the option says whether the item is usable and an upgrade (a fresh priest wears no cloak).
        expect("usable, upgrade" in greed_text, f"roll option lacks usable/upgrade facts: {greed_text!r}")
        expect("[Need rolled by Loada]" in greed_text,
               f"roll option doesn't say Loada rolled Need: {greed_text!r}")
        expect(not errors, f"Greed produced errors: {errors}")
        expect(options_gone, "the roll options stayed after Telda chose Greed")
        return f"loot_roll woke Telda {delay:.2f} s after the roll started; Greed accepted"
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_lfg_proposal():
    """A Dungeon Finder proposal wakes the brain while it is open (RH-H1).

    Uses AzerothCore's LFG testing mode (`Debug.LFG = 1` in the `party` test config), so a proposal
    forms for a group of two instead of a full tank/healer/3-dps five.

      1. Level Loada and Loadb to 20 and group them (Loada leads).
      2. Loada queues (`Queue Random Dungeon (LFG: Healer)`); Loadb confirms a non-healer role.
      3. Loadb gets an LFG_PROPOSAL prompt event and an `Accept Dungeon Finder proposal` option;
         both accept without an error, and the group is put into the dungeon (both bots' snapshots
         say `in an instance`).
    """

    import socket
    import sys
    import time

    from wlblib import server
    from wlblib.common import ROOT, TEST
    from wlblib.testing import expect


    NAME = "lfg_proposal"
    DESCRIPTION = "LFG proposal wakes the member's brain; both accept and enter the dungeon"
    TEST_CONFIG = "party"  # config/tests/party.*.overrides: 4 level-1 priests, Debug.LFG on.
    BOT = "Loada"
    ONLINE = 4






    def run(stack):
        for bot in ("Loada", "Loadb", "Loadc"):
            server.console(f"character level {bot} 20", inst=TEST)
        server.console("llmtest group Loada Loadb", inst=TEST)
        server.console("llmtest group Loada Loadc", inst=TEST)
        # Now #9: Dungeon Deserter on a party member blocks LFG queueing and
        # shows on the party frame.
        server.console("llmtest aura Loadb 71041", inst=TEST)

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()
        cmd_id = 0
        sent = {}
        errors = []
        prompts = {}  # bot -> set of prompt kinds
        role_at = None
        proposal_at = None
        in_instance = set()
        tele_tries = {}
        deserter_blocked = False
        deserter_cleared = False
        luck_seen = False
        boot_started = False
        boot_voted = False
        human_armed = False
        human_left = False
        out_after_leave = set()

        def choose(bot, idx, seq, step):
            nonlocal cmd_id
            cmd_id += 1
            sent[step] = cmd_id
            send_cmd(sock, {"bot": bot, "command_id": cmd_id,
                             "choose": {"option": idx, "seq": seq}})

        deadline = time.time() + 75.0
        try:
            while time.time() < deadline and len(out_after_leave) < 2:
                try:
                    chunk = sock.recv(1 << 20)
                except socket.timeout:
                    continue
                for payload in reader.feed(chunk):
                    msg = wire.decode_module_message(payload)
                    if not msg:
                        continue
                    if "event" in msg:
                        bot = msg["event"].get("bot")
                        ev = msg["event"].get("event", {})
                        err = ev.get("error")
                        # A refused teleport retry (still in combat) is retried, not a failure.
                        checked = {v for k, v in sent.items() if not k.startswith("tele_")}
                        if err and err.get("command_id") in checked:
                            errors.append(f"{bot}: {err.get('text')}")
                        if ev.get("prompt"):
                            kind = str(ev["prompt"].get("kind"))
                            prompts.setdefault(bot, set()).add(kind)
                            if bot == "Loadb" and kind == "LFG_PROPOSAL" and proposal_at is None:
                                proposal_at = time.time()
                        continue
                    snap = msg.get("snapshot")
                    if not snap:
                        continue
                    bot, seq = snap.get("bot", ""), snap.get("seq", 0)
                    options = snap.get("options", [])
                    is_in_inst = bool(snap.get("base", {}).get("where", {}).get("in_instance"))
                    if is_in_inst and not human_left:
                        in_instance.add(bot)
                    if human_left and bot in ("Loada", "Loadb") and not is_in_inst:
                        out_after_leave.add(bot)

                    if bot == "Loada" and not deserter_cleared:
                        members = snap.get("ext", {}).get("party", {}).get("members", [])
                        has_deserter_aura = any(
                            a.get("id") == 71041
                            for m in members if m.get("name") == "Loadb"
                            for a in m.get("auras", [])
                        )
                        if has_deserter_aura:
                            deserter_blocked = find_option(options, "Queue Random Dungeon") is None
                            deserter_cleared = True
                            server.console("llmtest unaura Loadb 71041", inst=TEST)
                        continue

                    if bot == "Loada" and "queue" not in sent:
                        idx = find_option(options, "Queue Random Dungeon (LFG: Healer)")
                        if idx:
                            choose("Loada", idx, seq, "queue")
                    elif bot in ("Loadb", "Loadc") and "queue" in sent and f"role_{bot}" not in sent:
                        idx = find_option(options, "Confirm Dungeon Finder role", avoid="Healer")
                        if idx:
                            choose(bot, idx, seq, f"role_{bot}")
                            role_at = time.time()
                    elif bot in ("Loada", "Loadb", "Loadc") and f"accept_{bot}" not in sent:
                        idx = find_option(options, "Accept Dungeon Finder proposal")
                        if idx:
                            choose(bot, idx, seq, f"accept_{bot}")
                    elif (bot in ("Loada", "Loadb", "Loadc") and bot not in in_instance
                          and not human_left and time.time() - tele_tries.get(bot, 0.0) > 5.0):
                        idx = find_option(options, "Teleport to LFG dungeon")
                        if idx:
                            tele_tries[bot] = time.time()
                            choose(bot, idx, seq, f"tele_{bot}_{int(tele_tries[bot])}")
                    elif len(in_instance) == 3 and not boot_started:
                        boot_started = True
                        server.console("llmtest aura Loada 72221", inst=TEST)
                        server.console("llmtest testhuman Loadc 1", inst=TEST)
                        server.console("llmtest lfgboot Loada Loadc afk", inst=TEST)
                    elif boot_started and not boot_voted and bot == "Loadb":
                        idx = find_option(options, "Vote not to kick Loadc")
                        if idx:
                            boot_voted = True
                            choose("Loadb", idx, seq, "boot_no")
                    elif boot_voted and bot == "Loadb" and not luck_seen:
                        members = snap.get("ext", {}).get("party", {}).get("members", [])
                        if any(a.get("id") == 72221 for m in members for a in m.get("auras", [])):
                            luck_seen = True
                    elif boot_voted and luck_seen and not human_armed:
                        human_armed = True
                        time.sleep(1.5)  # Let BotManager tick record human_group_guid.
                        human_left = True
                        server.console("llmtest ungroup Loadc", inst=TEST)
        finally:
            sock.close()
            server.console("llmtest testhuman Loadc 0", inst=TEST)
            server.console("llmtest unaura Loadb 71041", inst=TEST)
            server.console("llmtest unaura Loada 72221", inst=TEST)
            server.console("lfg clean", inst=TEST)
            for bot in ("Loada", "Loadb", "Loadc"):
                server.console(f"llmtest ungroup {bot}", inst=TEST)

        expect(deserter_blocked, "Dungeon Deserter on Loadb did not block `Queue Random Dungeon`")
        expect("queue" in sent, "Loada was never offered `Queue Random Dungeon (LFG: Healer)`")
        expect("role_Loadb" in sent and "role_Loadc" in sent,
               "members were not offered a non-healer `Confirm Dungeon Finder role`")
        expect(proposal_at is not None,
               f"Loadb got no LFG_PROPOSAL prompt event (prompts: {prompts})")
        delay = proposal_at - role_at
        expect(delay <= 10.0, f"LFG_PROPOSAL took {delay:.1f} s after the role check (> 10 s)")
        expect(all(f"accept_{b}" in sent for b in ("Loada", "Loadb", "Loadc")),
               "not all bots were offered `Accept Dungeon Finder proposal`")
        expect("LFG_BOOT_VOTE" in prompts.get("Loadb", set()),
               f"Loadb got no LFG_BOOT_VOTE prompt event (prompts: {prompts})")
        expect(boot_voted, "Loadb was never offered `Vote not to kick Loadc`")
        expect(luck_seen, "Luck of the Draw (72221) was not exposed on party members")
        expect(not errors, f"LFG choices produced errors: {errors}")
        expect(len(in_instance) == 3, f"not all bots entered the dungeon (in: {sorted(in_instance)})")
        expect(len(out_after_leave) == 2,
               f"bots did not teleport out after the human left (out: {sorted(out_after_leave)})")
        return (f"Deserter blocked queue; LFG_PROPOSAL woke Loadb in {delay:.2f} s; Luck of the Draw "
                "and boot vote worked; bots left and teleported out when the human left")
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_human_party():
    """A real human client plays with the bots (Review 2: T5, T9 single responder, T10).

    `humanparty` test config. A scripted 3.3.5a client (tools/wlblib/wowclient.py, no playerbots
    AI: a real player to the server) logs in as Humanone, an orc warrior at the Valley of Trials,
    next to Loada, Loadb and Loadc. The real harness runs the bots against a mock LLM that accepts
    group invites, keeps doing what it does, and answers a party line it was woken by.

      1. Humanone invites the three bots: each accepts, and playerbots makes Humanone their
         master (T5: follow, taxi follow and `!pb` commands work for the human).
      2. Humanone types "anyone up for RFC?" in party: exactly one bot is woken and answers, and
         the client sees that one party reply (the single responder, with a real client).
      3. A creature next to the party sends a raid boss emote: it reaches the bots and wakes them
         (T10).
    """

    import http.server
    import json
    import os
    import re
    import subprocess
    import tempfile
    import threading
    import time
    import pathlib

    from wlblib import build, server, wowclient
    from wlblib.common import OUT, ROOT, TEST
    from wlblib.testing import ProxyRecorder, expect

    NAME = "human_party"
    DESCRIPTION = "Real client: bots accept its invite, it becomes their master, one bot answers"
    TEST_CONFIG = "humanparty"
    BOT = "Loada"
    ONLINE = 3

    BOTS = ("Loada", "Loadb", "Loadc")
    HUMAN = "Humanone"
    ACCOUNT = "HUMANTEST"
    KEEP_RE = re.compile(r"^\s+(\d+)\. Keep doing what I'm doing$", re.M)
    ACCEPT_RE = re.compile(r"^\s+(\d+)\. Accept group invite", re.M)
    NAME_RE = re.compile(r"^You are (\w+)\.", re.M)

    class _Brain:
        def __init__(self):
            self.lock = threading.Lock()
            self.wakes = []  # (time, bot, woken line)
            self.prompts = []  # (time, bot, full prompt)
            parent = self

            class Handler(http.server.BaseHTTPRequestHandler):
                def log_message(self, fmt, *args):
                    pass

                def do_POST(self):
                    length = int(self.headers.get("Content-Length", "0"))
                    body = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
                    text = "\n".join(m.get("content", "") for m in body.get("messages", []))
                    name = NAME_RE.search(text)
                    bot = name.group(1) if name else "?"
                    woken = next((ln for ln in text.splitlines() if ln.startswith("Woken: ")), "")
                    with parent.lock:
                        parent.wakes.append((time.monotonic(), bot, woken))
                        parent.prompts.append((time.monotonic(), bot, text))
                    pick = ACCEPT_RE.search(text) or KEEP_RE.search(text)
                    lines = [f"do: {pick.group(1) if pick else 1}", "expect: none",
                             "watch: none", "look: 300"]
                    if f"party from {HUMAN}" in woken:
                        lines.append(f"party: {bot} here")
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

    def _account_ready():
        out = subprocess.run(
            ["mysql", f"--defaults-file={OUT / 'server' / 'etc' / 'mysql.cnf'}", "-uroot", "-N",
             "-e", f"select id from {TEST.db_prefix}auth.account where username = '{ACCOUNT}'"],
            capture_output=True, text=True, check=True).stdout.strip()
        return bool(out)

    def _poll(pred, timeout_s, what):
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            val = pred()
            if val:
                return val
            time.sleep(0.5)
        expect(False, f"timed out ({timeout_s} s) waiting for {what}")

    def run(stack):
        build.common("default", tests=False)
        if not _account_ready():
            server.console(f"account create {ACCOUNT} humantestpw", wait_s=1.0, inst=TEST)
            _poll(_account_ready, 20, "the human's account")
        key = os.urandom(40)
        wowclient.prepare_account(TEST.db_prefix, ACCOUNT, key)

        with tempfile.TemporaryDirectory(prefix="wlb-e2e-human-") as tmp:
            tmp_dir = pathlib.Path(tmp)
            proxy_sock = tmp_dir / "proxy.sock"
            proxy = ProxyRecorder(stack.socket, proxy_sock)
            brain = _Brain()
            proxy.start()
            brain.start()
            harness = subprocess.Popen(
                [str(OUT / "common" / "harness"), "run", f"--socket={proxy_sock}",
                 f"--rules={ROOT / 'content' / 'RULES.md'}", f"--bots-dir={tmp_dir / 'bots'}",
                 "--host=127.0.0.1", f"--port={brain.port}", "--slots=4", "--temperature=0"],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            client = None
            try:
                _poll(lambda: set(BOTS) <= {b for _, b, _ in brain.wakes}, 40,
                      "every bot's first look")
                client = wowclient.WowClient(TEST.world_port, ACCOUNT, key)
                client.authenticate()
                client.enter_world(HUMAN, race=2, cls=1)  # Orc warrior: Valley of Trials.
                time.sleep(2.0)

                # 1. Invites; playerbots makes the human their master.
                masters = {}
                for bot in BOTS:
                    client.invite(bot)
                    def is_master(bot=bot):
                        out = server.console(f"llmtest master {bot}", wait_s=0.5, inst=TEST)
                        m = re.search(rf"master {bot}=(\w+|-)", out)
                        masters[bot] = m.group(1) if m else "?"
                        return masters[bot] == HUMAN
                    _poll(is_master, 30, f"{bot} accepting the invite with {HUMAN} as master")

                # 2. One party line: one bot answers. (Now #8: the human's quest shows up
                # on the bots' screens as something to help with.)
                server.console(f"llmtest questadd {HUMAN} 788", wait_s=0.3, inst=TEST)
                time.sleep(2.0)
                since = time.monotonic()
                client.say(wowclient.CHAT_MSG_PARTY, "anyone up for RFC?")
                time.sleep(8.0)
                with brain.lock:
                    woken = sorted({b for t, b, w in brain.wakes
                                    if t >= since and f"party from {HUMAN}" in w})
                with brain.lock:
                    help_seen = any(t >= since and f"help {HUMAN} with Cutting Teeth" in txt
                                    for t, _, txt in brain.prompts)
                with client.lock:
                    replies = [c["text"] for c in client.chats
                               if c["t"] >= since and c["type"] == "party"
                               and c["from_guid"] != client.guid]  # Not its own echo.

                # 4 (Now #2). Leader commands apply within a tick, without the LLM.
                def stays(bot):
                    out = server.console(f"llmtest checkstrat {bot} stay", wait_s=0.3, inst=TEST)
                    m = re.search(rf"strat stay on {bot} exists=\d combat=\d noncombat=(\d)", out)
                    return m.group(1) == "1" if m else None
                client.say(wowclient.CHAT_MSG_PARTY, "wait here")
                _poll(lambda: all(stays(b) for b in BOTS), 5, "every bot staying after 'wait here'")
                client.say(wowclient.CHAT_MSG_PARTY, "loadb follow")
                _poll(lambda: stays("Loadb") is False, 5, "Loadb following after 'loadb follow'")
                still_wait = [b for b in ("Loada", "Loadc") if stays(b)]
                client.say(wowclient.CHAT_MSG_PARTY, "follow me")
                _poll(lambda: not any(stays(b) for b in BOTS), 5, "every bot following again")
                with proxy.lock:
                    applied = sorted({e["event"]["info"]["text"] for _, e in proxy.events
                                      if "leader said:" in e.get("event", {}).get("info", {})
                                      .get("text", "")})

                # 5 (Now #8). A follower 60 yd from its human leader, outdoors, comes back
                # by itself (no LLM call).
                server.console("llmtest tele Loadb 1 -560 -4250 40 0", wait_s=0.5, inst=TEST)
                _poll(lambda: "Loadb strayed" in stack.server_log(), 10,
                      "Loadb following back after straying")

                # 3. A boss emote reaches the bots.
                since_emote = time.monotonic()
                server.console(f"llmtest bossemote {HUMAN} roars at the party!", wait_s=0.5,
                               inst=TEST)
                _poll(lambda: any(t >= since_emote and "roars at the party" in w
                                  for t, _, w in list(brain.wakes)), 20,
                      "a bot woken by the boss emote")
            finally:
                if client:
                    client.close()
                harness.terminate()
                try:
                    harness.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    harness.kill()
                proxy.stop()
                brain.stop()
                for bot in BOTS:
                    server.console(f"llmtest ungroup {bot}", wait_s=0.2, inst=TEST)

        expect(all(m == HUMAN for m in masters.values()), f"masters: {masters}")
        expect(woken == ["Loada"], f"the party line woke {woken}, expected only Loada")
        expect(help_seen, f"no 'help {HUMAN} with Cutting Teeth' on the woken bot's screen")
        expect(replies == ["Loada here"], f"the client saw party replies {replies}")
        expect(still_wait == ["Loada", "Loadc"],
               f"'loadb follow' should move only Loadb; still waiting: {still_wait}")
        expect(any("leader said: wait (applied" in a for a in applied)
               and any("leader said: follow (applied" in a for a in applied),
               f"the brain wasn't told what was applied: {applied}")
        return (f"3 bots accepted, master={HUMAN} for all; one party reply ({replies[0]!r}); "
                "'wait here' / 'loadb follow' / 'follow me' applied in C++; quest help shown; "
                "stray follower came back; boss emote woke the bots")
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_popup_policy():
    """The brain's popup policy answers popups in C++, without a brain call (Now #3).

      1. Level Loada and Telda to 20 and group them (Loada leads).
      2. Telda picks `Answer popups automatically`.
      3. Loada starts a ready check with its `Start ready check` option (Now #5) and a group
         roll on a cape Telda can wear and has no cloak:
         Telda answers both by policy within seconds (ready; Greed: the cape binds on equip, and
         BoE is greeded even as an upgrade), and is told
         so by `popup policy:` info events, with no further choice from the brain.
    """

    import socket
    import time

    from wlblib import server
    from wlblib.common import TEST
    from wlblib.testing import expect

    NAME = "popup_policy"
    DESCRIPTION = "Answer popups automatically: ready check and loot roll answered in C++"
    TEST_CONFIG = "whisper"  # Two level-1 priests.
    BOT = "Loada"
    ONLINE = 2
    ITEM = 1355  # Buckskin Cape (green).

    def run(stack):
        for bot in ("Loada", "Telda"):
            server.console(f"character level {bot} 20", inst=TEST)
        server.console("llmtest group Loada Telda", inst=TEST)
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()
        policy_cmd = None
        policy_ok = False
        ready_started = False
        started = None
        answers = []
        chooses_after = 0
        deadline = time.time() + 40.0
        try:
            while time.time() < deadline and len(answers) < 2:
                try:
                    chunk = sock.recv(1 << 20)
                except socket.timeout:
                    chunk = b""
                for payload in reader.feed(chunk):
                    msg = wire.decode_module_message(payload)
                    if not msg:
                        continue
                    if "event" in msg:
                        if msg["event"].get("bot") != "Telda":
                            continue
                        ev = msg["event"].get("event", {})
                        text = ev.get("info", {}).get("text", "")
                        if text.startswith("popup policy:"):
                            answers.append(text)
                        if ev.get("error", {}).get("command_id") == policy_cmd:
                            expect(False, f"policy choice failed: {ev['error'].get('text')}")
                        continue
                    snap = msg.get("snapshot")
                    if not snap:
                        continue
                    if snap.get("bot") == "Loada":
                        # Now #5: the leader starts the ready check by its own option.
                        if policy_ok and not ready_started:
                            idx = find_option(snap.get("options", []), "Start ready check")
                            if idx:
                                ready_started = True
                                send_cmd(sock, {"bot": "Loada", "command_id": 8,
                                                 "choose": {"option": idx,
                                                            "seq": snap.get("seq", 0)}})
                        continue
                    if snap.get("bot") != "Telda":
                        continue
                    if policy_cmd is None:
                        idx = find_option(snap.get("options", []),
                                          "Answer popups automatically")
                        if idx:
                            policy_cmd = 7
                            send_cmd(sock, {"bot": "Telda", "command_id": policy_cmd,
                                             "choose": {"option": idx, "seq": snap.get("seq", 0)}})
                    elif not policy_ok and find_option(snap.get("options", []),
                                                       "Answer popups yourself again"):
                        policy_ok = True
                        started = time.time()
                        out = server.console(f"llmtest lootroll Loada {ITEM}", inst=TEST)
                        expect("roll started" in out, f"setup: no roll started ({out[-200:]})")
        finally:
            sock.close()
            server.console("llmtest ungroup Telda", inst=TEST)
        expect(policy_ok, "Telda never set the popup policy")
        expect(ready_started, "Loada (leader) was never offered `Start ready check`")
        took = time.time() - started if started else 0
        expect(any("answered the ready check: ready" in a for a in answers),
               f"ready check not answered by policy: {answers}")
        expect(any("rolled: Greed Buckskin Cape" in a for a in answers),
               f"loot roll not answered by policy (Greed: BoE): {answers}")
        return f"answered by policy within {took:.1f} s: {answers}"
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_crowd_control():
    """A mob marked for crowd control is offered to the CC class and polymorphed (Now #4).

      1. Loada (a troll mage made level 20 and taught Polymorph) groups with Telda.
      2. Telda's group marks a boar next to Loada with the moon.
      3. Loada is offered `Crowd-control Mottled Boar (moon): Polymorph`; choosing it puts
         Polymorph on the boar with no error; the bot manager sees the crowd control and turns
         area damage off near it.
    """

    import socket
    import time

    from wlblib import server
    from wlblib.common import TEST
    from wlblib.testing import expect

    NAME = "crowd_control"
    DESCRIPTION = "A moon-marked mob: the mage is offered Polymorph on it and it lands"
    TEST_CONFIG = "cc"
    BOT = "Loada"
    ONLINE = 2

    def run(stack):
        server.console("character level Loada 20", inst=TEST)
        server.console("llmtest learn Loada 118", inst=TEST)  # Polymorph (rank 1).
        server.console("llmtest group Telda Loada", inst=TEST)
        out = server.console("llmtest marknear Telda 4", inst=TEST)
        expect("marked" in out, f"setup: no marked boar ({out[-200:]})")
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()
        offered = ""
        sent = None
        errors = []
        polymorphed = False
        deadline = time.time() + 30.0
        try:
            while time.time() < deadline and not polymorphed:
                try:
                    chunk = sock.recv(1 << 20)
                except socket.timeout:
                    continue
                for payload in reader.feed(chunk):
                    msg = wire.decode_module_message(payload)
                    if not msg:
                        continue
                    if "event" in msg:
                        err = msg["event"].get("event", {}).get("error")
                        if err and sent is not None and err.get("command_id") == sent:
                            errors.append(err.get("text"))
                        continue
                    snap = msg.get("snapshot")
                    if not snap or snap.get("bot") != "Loada":
                        continue
                    if sent is None:
                        idx = find_option(snap.get("options", []), "Crowd-control Mottled Boar")
                        if idx:
                            offered = snap["options"][idx - 1].get("text", "")
                            sent = 5
                            send_cmd(sock, {"bot": "Loada", "command_id": sent,
                                             "choose": {"option": idx, "seq": snap.get("seq", 0)}})
                    elif "Loada: crowd control nearby" in stack.server_log():
                        # The bot manager saw the boar held by breakable CC (and turned
                        # Loada's and Telda's area damage off for it).
                        polymorphed = True
        finally:
            sock.close()
            server.console("llmtest ungroup Loada", inst=TEST)
        expect(offered == "Crowd-control Mottled Boar (moon): Polymorph",
               f"CC option: {offered!r}")
        expect(not errors, f"Polymorph failed: {errors}")
        expect(polymorphed, "no crowd control seen near Loada after Polymorph")
        return f"offered {offered!r}; the boar was polymorphed"
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

CASES = [_case_crowd_control(), _case_popup_policy(), _case_human_party(), _case_invite(), _case_group(), _case_tank(), _case_popups(), _case_rez_ready(), _case_lootroll(), _case_lfg_proposal()]
