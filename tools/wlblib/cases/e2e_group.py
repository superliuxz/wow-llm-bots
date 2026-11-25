"""Grouped e2e cases: invite, group, tank, pull_gate, popups, rez_ready, lootroll, lfg_proposal,
crisp_dodge."""

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
    AI: a real player to the server) logs in as Allyone, a human warrior at Northshire Abbey,
    next to Loada, Loadb and Loadc. The real harness runs the bots against a mock LLM that accepts
    group invites, keeps doing what it does, and answers a party line it was woken by.

      1. Allyone invites the three bots: each accepts, and playerbots makes Allyone their
         master (T5: follow, taxi follow and `!pb` commands work for the human).
      2. Allyone types "anyone up for Deadmines?" in party: exactly one bot is woken and answers,
         and the client sees that one party reply (the single responder, with a real client).
      3. A creature next to the party sends a raid boss emote: it reaches the bots and wakes them
         (T10).
    """

    import http.server
    import json
    import math
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
    HUMAN = "Allyone"
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
                    # Milestone 1d: Loada answers the boss emote by going to the ping
                    # on its screen (a plan `act:` line, sent ahead of its `do:`).
                    if bot == "Loada" and "roars at the party" in woken and "\nping: " in text:
                        lines.append("act: to ping")
                    # ... and `use` walks into the spell's range before casting.
                    if (bot == "Loadb" and f"party from {HUMAN}" in woken
                            and "loadb fort me please" in text):
                        lines.append("act: use Power Word: Fortitude on leader")
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
                # Adversarial counter-test: CharacterCreating.Disabled = 2 blocks Horde creation.
                horde_blocked = False
                try:
                    client.create_character("Hordeblock", race=2, cls=1)
                except ConnectionError as exc:
                    horde_blocked = "character create refused" in str(exc)
                expect(horde_blocked, "Horde character creation was not blocked on Alliance realm")
                client.enter_world(HUMAN, race=1, cls=1)  # Human warrior: Northshire Abbey.
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
                server.console(f"llmtest questadd {HUMAN} 7", wait_s=0.3, inst=TEST)
                time.sleep(2.0)
                since = time.monotonic()
                client.say(wowclient.CHAT_MSG_PARTY, "anyone up for Deadmines?")
                time.sleep(8.0)
                with brain.lock:
                    woken = sorted({b for t, b, w in brain.wakes
                                    if t >= since and f"party from {HUMAN}" in w})
                with brain.lock:
                    help_seen = any(t >= since and f"help {HUMAN} with Kobold Camp Cleanup" in txt
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

                # 6 (Milestone 1b). A minimap ping from the human shows on every bot's
                # minimap (`ext.minimap.ping`), relative to the bot; a bot's own ping
                # reaches the others and the human, never itself.
                def pings_of(bot, after, before=None):
                    with proxy.lock:
                        snaps = [(t, s) for t, s in proxy.snapshots
                                 if s.get("bot") == bot and t >= after
                                 and (before is None or t < before)]
                    return [(t, s, ((s.get("ext") or {}).get("minimap") or {}).get("ping"))
                            for t, s in snaps]
                start_t = 0.0
                early_pings = [p for b in BOTS for _, _, p in pings_of(b, start_t) if p]
                with proxy.lock:
                    loada_now = [s for _, s in proxy.snapshots if s.get("bot") == "Loada"][-1]
                lpos = loada_now["base"]["where"]["position"]
                since_ping = time.monotonic()
                client.ping(lpos["x"] + 15.0, lpos["y"])  # 15 yd north of Loada.

                def all_see_human_ping():
                    return all(any(p and p.get("from") == HUMAN for _, _, p in
                                   pings_of(b, since_ping)) for b in BOTS)
                _poll(all_see_human_ping, 10, f"every bot seeing {HUMAN}'s minimap ping")
                loada_ping = next(p for _, _, p in pings_of("Loada", since_ping)
                                  if p and p.get("from") == HUMAN)
                since_bot_ping = time.monotonic()
                server.console("llmtest ping Loada 0 10", wait_s=0.3, inst=TEST)
                _poll(lambda: any(p and p.get("from") == "Loada" for _, _, p in
                                  pings_of("Loadb", since_bot_ping)), 10,
                      "Loadb seeing Loada's minimap ping")
                time.sleep(1.5)
                self_pinged = [p for _, _, p in pings_of("Loada", since_bot_ping)
                               if p and p.get("from") == "Loada"]
                with client.lock:
                    human_saw = [pg for pg in client.pings if pg["t"] >= since_bot_ping
                                 and pg["from_guid"] != client.guid]

                # 5 (Now #8). A follower 60 yd from its human leader, outdoors, comes back
                # by itself (no LLM call).
                server.console("llmtest tele Loadb 0 -8880 -132.5 82 0", wait_s=0.5, inst=TEST)
                _poll(lambda: "Loadb strayed" in stack.server_log(), 10,
                      "Loadb following back after straying")

                # 3. A boss emote reaches the bots (and, 6, the woken bot's screen shows a
                # fresh ping).
                client.ping(lpos["x"], lpos["y"] + 12.0)
                time.sleep(1.5)
                since_emote = time.monotonic()
                server.console(f"llmtest bossemote {HUMAN} roars at the party!", wait_s=0.5,
                               inst=TEST)
                _poll(lambda: any(t >= since_emote and "roars at the party" in w
                                  for t, _, w in list(brain.wakes)), 20,
                      "a bot woken by the boss emote")
                with brain.lock:
                    ping_on_screen = any(t >= since_emote and f"\nping: {HUMAN} pinged " in txt
                                         for t, _, txt in brain.prompts)

                # 7 (Milestone 1d). Loada's plan `act: to ping` walks it to the ping, and it
                # holds there (playerbots' `stay`) while its leader is near; the other bots
                # keep following; "follow me" ends the hold.
                ping_spot = (lpos["x"], lpos["y"] + 12.0)

                def to_ping_cmd():
                    with proxy.lock:
                        return next((c for t, c in proxy.commands
                                     if t >= since_emote and c.get("bot") == "Loada"
                                     and (c.get("act") or {}).get("verb") == "TO"
                                     and (c.get("act") or {}).get("thing") == "PING"), None)
                ping_cmd = _poll(to_ping_cmd, 20, "Loada's plan act `to ping`")

                def loada_at_ping():
                    with proxy.lock:
                        snaps = [s for t, s in proxy.snapshots
                                 if s.get("bot") == "Loada" and t >= since_emote]
                    for s in reversed(snaps):
                        pos = s["base"]["where"]["position"]
                        if math.dist((pos["x"], pos["y"]), ping_spot) <= 4.0:
                            return s
                    return None
                _poll(loada_at_ping, 20, "Loada reaching the ping")
                with proxy.lock:
                    act_done = any(s.get("bot") == "Loada"
                                   and s.get("done_act_command_id") == ping_cmd["command_id"]
                                   for _, s in proxy.snapshots)
                held_at_ping = bool(_poll(lambda: stays("Loada"), 5,
                                          "Loada holding at the ping"))
                others_follow = [b for b in ("Loadb", "Loadc") if stays(b)]
                time.sleep(2.0)
                with proxy.lock:
                    last = [s for _, s in proxy.snapshots if s.get("bot") == "Loada"][-1]
                lp = last["base"]["where"]["position"]
                still_at_ping = math.dist((lp["x"], lp["y"]), ping_spot) <= 5.0
                client.say(wowclient.CHAT_MSG_PARTY, "follow me")
                _poll(lambda: stays("Loada") is False, 5, "Loada following after 'follow me'")

                # 8 (Milestone 1d). `use` walks into the spell's range first: Loadb waits
                # 60+ yd from its leader (Power Word: Fortitude reaches 30), is told to
                # fort the leader, walks to 2 yd inside that range and casts. (Not a heal:
                # playerbots cancels a heal on a full-health target.)
                def human_in(snap):
                    ext_party = ((snap.get("ext") or {}).get("party") or {}).get("members", [])
                    units = ((snap.get("base") or {}).get("nearby") or {}).get("units", [])
                    return next((u for u in list(ext_party) + list(units)
                                 if u.get("name") == HUMAN), None)

                def latest(bot, after=0.0):
                    with proxy.lock:
                        return next((s for t, s in reversed(proxy.snapshots)
                                     if s.get("bot") == bot and t >= after), None)
                server.console("llmtest learn Loadb 1243", wait_s=0.5, inst=TEST)
                client.say(wowclient.CHAT_MSG_PARTY, "wait here")
                _poll(lambda: stays("Loadb"), 5, "Loadb staying after 'wait here'")
                hpos = human_in(latest("Loadb"))["rel"]["position"]
                # The stray spot of step 5 (its ground height is known): 60+ yd away.
                server.console("llmtest tele Loadb 0 -8880 -132.5 82 0", wait_s=1.0, inst=TEST)
                since_tele = time.monotonic()

                def loadb_far():
                    s = latest("Loadb", since_tele)
                    pos = s and s["base"]["where"]["position"]
                    return pos and math.dist((pos["x"], pos["y"]), (hpos["x"], hpos["y"])) > 31.0
                _poll(loadb_far, 10, "Loadb waiting out of Power Word: Fortitude's range")
                since_fort = time.monotonic()
                client.say(wowclient.CHAT_MSG_PARTY, "loadb fort me please")

                def fort_cmd():
                    with proxy.lock:
                        return next((c for t, c in proxy.commands
                                     if t >= since_fort and c.get("bot") == "Loadb"
                                     and (c.get("act") or {}).get("verb") == "USE"), None)
                fort = _poll(fort_cmd, 20, "Loadb's plan act `use Power Word: Fortitude`")

                def fort_done():
                    with proxy.lock:
                        return next((s for _, s in proxy.snapshots
                                     if s.get("bot") == "Loadb"
                                     and s.get("done_act_command_id") == fort["command_id"]),
                                    None)
                done_snap = _poll(fort_done, 25,
                                  "Loadb walking in and casting Power Word: Fortitude")
                with proxy.lock:
                    fort_errs = [e["event"]["error"] for _, e in proxy.events
                                 if (e.get("event", {}).get("error") or {}).get("command_id")
                                 == fort["command_id"]]

                bpos = done_snap["base"]["where"]["position"]
                fort_dist = math.dist((bpos["x"], bpos["y"]), (hpos["x"], hpos["y"]))
                client.say(wowclient.CHAT_MSG_PARTY, "follow me")
                _poll(lambda: not any(stays(b) for b in BOTS), 5, "every bot following again")

                # 9 (Milestone 2b). Raid icon tags reach the bots as words ("{rt7}" ->
                # "cross"), and the fast leader shortcuts run in C++ with no LLM call:
                # "kill {rt7}" with nothing marked is refused, "go there" walks the bots
                # to a fresh ping and holds them, "los" runs them back to the speaker
                # and holds them there, "follow me" ends it.
                def infos_since(after):
                    with proxy.lock:
                        return [(e.get("bot"), e["event"]["info"]["text"])
                                for t, e in proxy.events if t >= after
                                and "leader said:" in (e.get("event", {}).get("info") or {})
                                .get("text", "")]

                def snap_near(bot, spot, yd, after):
                    s = latest(bot, after)
                    pos = s and s["base"]["where"]["position"]
                    return bool(pos) and math.dist((pos["x"], pos["y"]), spot) <= yd
                since_rt = time.monotonic()
                client.say(wowclient.CHAT_MSG_PARTY, "kill {rt7}")
                cross_refused = bool(_poll(
                    lambda: any("leader said: kill cross (not applied: nothing marked cross)"
                                in t for _, t in infos_since(since_rt)), 5,
                    "'kill {rt7}' refused in C++ with nothing marked cross"))
                time.sleep(1.0)
                with brain.lock:
                    rt_prompts = [txt for t, _, txt in brain.prompts if t >= since_rt]
                rt_as_word = (any("kill cross" in txt for txt in rt_prompts)
                              and not any("{rt7}" in txt for txt in rt_prompts))

                hp = human_in(latest("Loada"))["rel"]["position"]
                human_spot = (hp["x"], hp["y"])
                client.ping(*ping_spot)
                time.sleep(1.5)
                since_go = time.monotonic()
                client.say(wowclient.CHAT_MSG_PARTY, "go there")
                went_there = bool(_poll(
                    lambda: all(snap_near(b, ping_spot, 4.0, since_go)
                                for b in ("Loada", "Loadc")), 15,
                    "Loada and Loadc at the ping after 'go there'"))
                held_there = bool(_poll(lambda: stays("Loada") and stays("Loadc"), 5,
                                        "Loada and Loadc holding at the ping"))
                since_los = time.monotonic()
                client.say(wowclient.CHAT_MSG_PARTY, "los")
                came_back = bool(_poll(
                    lambda: all(snap_near(b, human_spot, 5.0, since_los)
                                for b in ("Loada", "Loadc")), 15,
                    f"Loada and Loadc back at {HUMAN} after 'los'"))
                held_los = bool(_poll(lambda: stays("Loada") and stays("Loadc"), 5,
                                      f"Loada and Loadc holding at {HUMAN} after 'los'"))
                shortcut_infos = infos_since(since_go)
                client.say(wowclient.CHAT_MSG_PARTY, "follow me")
                _poll(lambda: not any(stays(b) for b in BOTS), 5,
                      "every bot following again after 'los'")

                # 10 (Milestone 3a). The human's /afk shows on the bots' party frames
                # (`group.afk`), and clears when they're back.
                def human_afk(after):
                    s = latest("Loadc", after)
                    m = s and human_in(s)
                    return m and bool((m.get("group") or {}).get("afk"))
                since_afk = time.monotonic()
                client.say(wowclient.CHAT_MSG_AFK, "")
                went_afk = bool(_poll(lambda: human_afk(since_afk), 10,
                                      f"{HUMAN} shown AFK to Loadc"))
                since_back = time.monotonic()
                client.say(wowclient.CHAT_MSG_AFK, "")
                came_back_afk = bool(_poll(
                    lambda: latest("Loadc", since_back) and not human_afk(since_back), 10,
                    f"{HUMAN} no longer shown AFK to Loadc"))

                # 6. Leaving the group takes the ping off the minimap at once.
                before_ungroup = pings_of("Loadc", since_emote)
                pinged_before_ungroup = bool(before_ungroup and before_ungroup[-1][2])
                since_ungroup = time.monotonic()
                server.console("llmtest ungroup Loadc", wait_s=0.3, inst=TEST)
                time.sleep(0.5)

                def ungrouped_snaps():
                    return [p for _, _, p in pings_of("Loadc", since_ungroup + 1.0)]
                _poll(lambda: len(ungrouped_snaps()) >= 2, 10, "Loadc snapshots after ungroup")
                ungrouped_pings = [p for p in ungrouped_snaps() if p]
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
        expect(help_seen, f"no 'help {HUMAN} with Kobold Camp Cleanup' on the woken bot's screen")
        expect(replies == ["Loada here"], f"the client saw party replies {replies}")
        expect(still_wait == ["Loada", "Loadc"],
               f"'loadb follow' should move only Loadb; still waiting: {still_wait}")
        expect(any("leader said: wait (applied" in a for a in applied)
               and any("leader said: follow (applied" in a for a in applied),
               f"the brain wasn't told what was applied: {applied}")
        expect(not early_pings, f"a minimap ping before anyone pinged: {early_pings}")
        rel = loada_ping.get("rel") or {}
        expect(10.0 <= rel.get("dist_yd", 0.0) <= 20.0 and rel.get("dir") in ("N", "NE", "NW"),
               f"Loada saw {HUMAN}'s ping 15 yd north as {loada_ping}")
        expect(not self_pinged, f"Loada saw its own ping: {self_pinged}")
        expect(human_saw, f"{HUMAN}'s client never saw Loada's ping")
        expect(ping_on_screen, "no 'ping: Allyone pinged ...' line on a woken bot's screen")
        expect(pinged_before_ungroup, "Loadc had no fresh ping just before leaving the group")
        expect(not ungrouped_pings, f"Loadc kept a ping after leaving the group: "
               f"{ungrouped_pings}")
        expect(act_done, f"Loada's `to ping` act {ping_cmd} never finished")
        expect(held_at_ping and still_at_ping,
               f"Loada didn't hold at the ping (stay={held_at_ping}, at={lp})")
        expect(not others_follow, f"bots without the act stopped following: {others_follow}")
        expect(not fort_errs, f"Loadb's walk-in cast was refused: {fort_errs}")
        expect(went_afk and came_back_afk,
               f"{HUMAN}'s /afk wasn't shown and cleared (afk={went_afk}, "
               f"back={came_back_afk})")
        expect(cross_refused, "'kill {rt7}' wasn't parsed as kill cross")
        expect(rt_as_word, "the brain saw '{rt7}' instead of 'cross'")
        expect(went_there and held_there,
               f"'go there' didn't walk and hold the bots at the ping "
               f"(at={went_there}, held={held_there})")
        expect(came_back and held_los,
               f"'los' didn't bring the bots back to {HUMAN} and hold them "
               f"(at={came_back}, held={held_los})")
        expect(any("leader said: go there (applied" in t for _, t in shortcut_infos)
               and any("leader said: pull back (applied" in t for _, t in shortcut_infos),
               f"the brain wasn't told the shortcuts were applied: {shortcut_infos}")
        # 30 yd range, edge to edge (+ ~3 yd of combat reach): 2 yd inside it.
        expect(fort_dist <= 31.5, f"Loadb cast Power Word: Fortitude from {fort_dist:.1f} yd")
        return (f"3 bots accepted, master={HUMAN} for all; one party reply ({replies[0]!r}); "
                "'wait here' / 'loadb follow' / 'follow me' applied in C++; quest help shown; "
                "stray follower came back; boss emote woke the bots; minimap pings shown "
                "(`act: to ping` walked Loada there and held it; `use` walked Loadb into "
                "range first; 'kill {rt7}' read as cross, 'go there' / 'los' applied in C++) "
                f"({rel.get('dist_yd', 0):.0f} yd {rel.get('dir')}), not to self, gone on ungroup")
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_pull_gate():
    """Who may pull: the gate on Attack options for mobs nobody fights yet (Milestone 3a).

    Loada stands in for the human (`llmtest testhuman Loada 1`) and leads Telda. Telda's
    snapshots say which mobs it may pull (an `Attack ... (not pulled)` option); the brain picks.

      1. Solo, Telda is offered Attack on the mobs around (so there are some).
      2. Grouped under the human leader, Telda (no tank) gets no unpulled Attack.
      3. Made the group's tank (`llmtest maketank`), Telda is offered them, marked
         `(not pulled)`: the human leader doesn't tank.
      4. The human 40 yd away (behind): no unpulled Attack again.
      5. Counter-test: back next to Telda, they are offered again.
    """

    import socket
    import time

    from wlblib import server
    from wlblib.common import TEST
    from wlblib.testing import expect

    NAME = "pull_gate"
    DESCRIPTION = "A bot tank under a human leader may pull; not when the human is behind"
    TEST_CONFIG = "orders"  # config/tests/orders.*.overrides: two priests next to boars.
    BOT = "Telda"
    ONLINE = 2
    KALIMDOR = 1

    def run(stack):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()
        seen = {}
        pos = None

        def next_telda(after, timeout_s=20.0):
            deadline = time.time() + timeout_s
            while time.time() < deadline:
                try:
                    chunk = sock.recv(1 << 20)
                except socket.timeout:
                    continue
                for payload in reader.feed(chunk):
                    msg = wire.decode_module_message(payload)
                    snap = (msg or {}).get("snapshot")
                    if snap and snap.get("bot") == "Telda" and time.time() >= after:
                        return snap
            return None

        def wait_for(step, pred, after, timeout_s=20.0):
            deadline = time.time() + timeout_s
            while time.time() < deadline:
                snap = next_telda(after, deadline - time.time())
                if snap is None:
                    break
                texts = [o.get("text", "") for o in snap.get("options", [])]
                seen[step] = texts
                if pred(snap, texts):
                    return True
            return False

        def grouped(snap):
            return bool(((snap.get("ext") or {}).get("party") or {}).get("members"))

        def unpulled(texts):
            return [t for t in texts if t.startswith("Attack ") and t.endswith("(not pulled)")]

        try:
            t = time.time()
            solo = wait_for("solo", lambda s, x: not grouped(s)
                            and any(o.startswith("Attack ") for o in x), t)
            server.console("llmtest testhuman Loada 1", wait_s=0.3, inst=TEST)
            server.console("llmtest group Loada Telda", wait_s=0.5, inst=TEST)
            t = time.time()
            held_no_tank = (wait_for("held", lambda s, x: grouped(s), t)
                            and wait_for("held2", lambda s, x: grouped(s), time.time())
                            and not unpulled(seen["held"]) and not unpulled(seen["held2"]))
            server.console("llmtest maketank Telda", wait_s=1.5, inst=TEST)
            tank_may_pull = wait_for("tank", lambda s, x: grouped(s) and unpulled(x),
                                     time.time())
            snap = next_telda(time.time())
            pos = snap["base"]["where"]["position"]
            server.console(f"llmtest tele Loada {KALIMDOR} {pos['x'] + 40.0} {pos['y']} "
                           f"{pos['z'] + 1.0} 0", wait_s=1.0, inst=TEST)
            behind_held = wait_for("behind", lambda s, x: grouped(s) and not unpulled(x),
                                   time.time())
            server.console(f"llmtest tele Loada {KALIMDOR} {pos['x'] + 2.0} {pos['y']} "
                           f"{pos['z'] + 1.0} 0", wait_s=1.0, inst=TEST)
            back_may_pull = wait_for("back", lambda s, x: grouped(s) and unpulled(x),
                                     time.time())
        finally:
            sock.close()
            if pos:
                server.console(f"llmtest tele Loada {KALIMDOR} {pos['x']} {pos['y']} "
                               f"{pos['z'] + 1.0} 0", wait_s=0.5, inst=TEST)
            server.console("llmtest ungroup Telda", inst=TEST)
            server.console("llmtest ungroup Loada", inst=TEST)
            server.console("llmtest testhuman Loada 0", inst=TEST)
            server.console("llmtest clearstrat", inst=TEST)

        expect(solo, f"solo Telda was offered no Attack (options: {seen.get('solo')})")
        expect(held_no_tank, f"Telda (not a tank) under a human leader was offered unpulled "
               f"mobs: {seen.get('held')} / {seen.get('held2')}")
        expect(tank_may_pull, f"Telda as the group's tank was offered no unpulled mob "
               f"(options: {seen.get('tank')})")
        expect(behind_held, f"Telda was offered unpulled mobs with the human 40 yd behind: "
               f"{seen.get('behind')}")
        expect(back_may_pull, f"Telda wasn't offered unpulled mobs again with the human back "
               f"(options: {seen.get('back')})")
        return (f"tank Telda offered {unpulled(seen['tank'])[0]!r}; none as a non-tank or with "
                "the human 40 yd behind")
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
        manual_cmd = None
        manual_ok = False
        ready_started = False
        started = None
        answers = []
        chooses_after = 0
        deadline = time.time() + 40.0
        try:
            while time.time() < deadline and not manual_ok:
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
                        cid = ev.get("error", {}).get("command_id")
                        if cid is not None and cid in (policy_cmd, manual_cmd):
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
                    elif len(answers) >= 2 and manual_cmd is None:
                        idx = find_option(snap.get("options", []),
                                          "Answer popups yourself again")
                        if idx:
                            manual_cmd = 9
                            send_cmd(sock, {"bot": "Telda", "command_id": manual_cmd,
                                             "choose": {"option": idx, "seq": snap.get("seq", 0)}})
                    elif manual_cmd is not None and find_option(snap.get("options", []),
                                                                "Answer popups automatically"):
                        manual_ok = True
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
        expect(manual_ok, "Telda never turned automatic popups back off")
        return f"answered by policy within {took:.1f} s and restored manual popups: {answers}"
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

def _case_use_physics():
    """`use` physics from Spell.dbc on the option and act paths (Milestone 1d).

      1. Loada (a troll rogue made level 10, taught Stealth and Sap) groups with Telda, whose
         group square-marks a boar and moon-marks a passive Defias Thug, 20 yd from Loada (Sap
         reaches 10). Sap (rank 1 holds humanoids only) is offered on the thug, not the boar.
         Choosing `Crowd-control Defias Thug (moon): Sap` enters Stealth first, walks into range,
         turns to face the thug and Saps it: the option finishes (done_command_id), no error.
      2. Loadb (an orc warlock made level 20, taught Rain of Fire) is sent `act: use Rain of Fire
         on ping` for a party ping 38 yd off (the spell reaches 30): it walks in and channels it
         on the spot.
      3. With its imp out (`act: use Summon Imp` unless it has one), `act: pet dismiss` sends it
         away (the pet bar's dismiss: a warlock knows no Dismiss Pet).
    """

    import math
    import socket
    import time

    from wlblib import server
    from wlblib.common import TEST
    from wlblib.testing import expect

    NAME = "use_physics"
    DESCRIPTION = "Stealth->Sap walk-in (CC option), a ground spell on the ping, pet dismiss"
    TEST_CONFIG = "usephys"
    BOT = "Loada"
    ONLINE = 3

    def run(stack):
        server.console("character level Loada 10", inst=TEST)
        server.console("llmtest learn Loada 1784", inst=TEST)  # Stealth.
        server.console("llmtest learn Loada 6770", inst=TEST)  # Sap (rank 1).
        server.console("character level Loadb 20", inst=TEST)
        server.console("llmtest learn Loadb 5740", inst=TEST)  # Rain of Fire (rank 1).
        server.console("llmtest learn Loadb 688", inst=TEST)  # Summon Imp.
        server.console("llmtest group Telda Loada", inst=TEST)
        server.console("llmtest group Telda Loadb", inst=TEST)
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()
        snaps = []   # (monotonic, snapshot)
        errors = {}  # command_id -> [text]

        def pump(until, timeout, what):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                got = until()
                if got:
                    return got
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
                        if err:
                            errors.setdefault(err.get("command_id"), []).append(err.get("text"))
                        continue
                    if msg.get("snapshot"):
                        snaps.append((time.monotonic(), msg["snapshot"]))
            raise AssertionError(f"timed out waiting for {what} (errors: {errors})")

        def latest(bot, after=0.0):
            return next((s for t, s in reversed(snaps)
                         if s.get("bot") == bot and t >= after), None)

        def boar_of(snap):
            return next((u for u in snap["base"]["nearby"]["units"]
                         if u.get("name") == "Defias Thug" and u.get("raid_marker") == "MOON"),
                        None)

        def done(bot, field, cmd_id):
            return lambda: (cmd_id in errors) or next(
                (s for _, s in snaps if s.get("bot") == bot and s.get(field) == cmd_id), None)

        def pet_of(snap):
            # Nobody else in the party has a pet: a pet in sight is Loadb's imp.
            return next((u for u in snap["base"]["nearby"]["units"]
                         if u.get("kind") == "PET" and not u.get("dead")), None)

        try:
            # 1. Stealth -> Sap through the crowd-control option. Loada stands 8 yd west of
            #    Telda, so the boar (12 yd east of Telda) is out of Sap's reach.
            def telda_seen():
                s = latest("Loada")
                return s and next((u for u in s["base"]["nearby"]["units"]
                                   if u.get("name") == "Telda"), None)
            tpos = pump(telda_seen, 20, "Telda in Loada's sight")["rel"]["position"]
            server.console(f"llmtest move Loada {tpos['x'] - 8.0} {tpos['y']} {tpos['z'] + 0.5} 0",
                           wait_s=0.5, inst=TEST)
            # A boar (square) and a passive Defias Thug (moon): rank-1 Sap holds humanoids only.
            out = server.console("llmtest marknear Telda 5", inst=TEST)
            expect("marked" in out, f"setup: no marked boar ({out[-200:]})")
            out = server.console("llmtest marknear Telda 4 38", inst=TEST)
            expect("marked" in out, f"setup: no marked thug ({out[-200:]})")
            since_mark = time.monotonic()

            def cc_offer():
                s = latest("Loada", since_mark)
                idx = s and find_option(s.get("options", []), "Crowd-control Defias Thug")
                return idx and (s, idx)
            snap, idx = pump(cc_offer, 20, "Loada's crowd-control option")
            offered = snap["options"][idx - 1].get("text", "")
            boar_offered = [o.get("text") for o in snap.get("options", [])
                            if o.get("text", "").startswith("Crowd-control Mottled Boar")]
            sap_from = boar_of(snap)["rel"]["dist_yd"]
            send_cmd(sock, {"bot": "Loada", "command_id": 11,
                            "choose": {"option": idx, "seq": snap.get("seq", 0)}})
            pump(done("Loada", "done_command_id", 11), 25, "Loada's Sap to finish")
            sap_errs = errors.get(11, [])
            sapped = bool(pump(lambda: "Loada: crowd control nearby" in stack.server_log(), 5,
                               "the bot manager seeing the Sap")) if not sap_errs else False
            boar = boar_of(latest("Loada"))
            sap_at = boar["rel"]["dist_yd"] if boar else None

            # 2. A ground spell on the party's ping, out of range: walk in, then cast.
            server.console("llmtest ping Telda 0 38", wait_s=0.5, inst=TEST)
            since_ping = time.monotonic()

            def ping_seen():
                s = latest("Loadb", since_ping)
                return s and (((s.get("ext") or {}).get("minimap") or {}).get("ping"))
            ping = pump(ping_seen, 10, "the ping on Loadb's minimap")
            rof_from = ping["rel"]["dist_yd"]
            send_cmd(sock, {"bot": "Loadb", "command_id": 21,
                            "act": {"verb": "USE", "name": "Rain of Fire", "thing": "PING"}})
            pump(done("Loadb", "done_act_command_id", 21), 30, "Loadb's Rain of Fire to finish")
            rof_errs = errors.get(21, [])
            rof_snap = next((s for _, s in snaps if s.get("bot") == "Loadb"
                             and s.get("done_act_command_id") == 21), None)
            rof_ping = rof_snap and (((rof_snap.get("ext") or {}).get("minimap") or {})
                                     .get("ping"))
            rof_at = rof_ping["rel"]["dist_yd"] if rof_ping else None

            # 3. Pet dismiss.
            summon_errs = []
            if not pet_of(latest("Loadb")):
                send_cmd(sock, {"bot": "Loadb", "command_id": 31,
                                "act": {"verb": "USE", "name": "Summon Imp"}})
                pump(lambda: pet_of(latest("Loadb")) or 31 in errors, 25, "Loadb's imp")
                summon_errs = errors.get(31, [])
            had_pet = bool(pet_of(latest("Loadb")))
            since_dismiss = time.monotonic()
            send_cmd(sock, {"bot": "Loadb", "command_id": 32,
                            "act": {"verb": "PET", "pet_cmd": "DISMISS"}})
            pump(done("Loadb", "done_act_command_id", 32), 10, "Loadb's pet dismiss")
            dismiss_errs = errors.get(32, [])
            pump(lambda: latest("Loadb", since_dismiss) and not pet_of(
                latest("Loadb", since_dismiss)), 5, "Loadb's imp gone")
        finally:
            sock.close()
            server.console("llmtest ungroup Loada", inst=TEST)
            server.console("llmtest ungroup Loadb", inst=TEST)
        expect(offered == "Crowd-control Defias Thug (moon): Sap", f"CC option: {offered!r}")
        expect(not boar_offered, f"Sap offered on a beast it can't hold: {boar_offered!r}")
        expect(sap_from > 10.0, f"setup: Loada started {sap_from:.1f} yd from the thug")
        expect(not sap_errs, f"Sap failed: {sap_errs}")
        expect(sapped, "no crowd control seen near Loada after Sap")
        expect(sap_at is not None and sap_at <= 10.0,
               f"Loada Sapped from {sap_at} yd (Sap reaches 10)")
        expect(rof_from > 30.0, f"setup: the ping was {rof_from:.1f} yd from Loadb")
        expect(not rof_errs, f"Rain of Fire on the ping failed: {rof_errs}")
        expect(rof_at is not None and rof_at <= 30.0,
               f"Loadb cast Rain of Fire {rof_at} yd from the ping (it reaches 30)")
        expect(not summon_errs and had_pet, f"no imp to dismiss: {summon_errs}")
        expect(not dismiss_errs, f"pet dismiss failed: {dismiss_errs}")
        return (f"stealth->Sap walked in from {sap_from:.0f} to {sap_at:.0f} yd; Rain of Fire on "
                f"the ping from {rof_at:.0f} yd (was {rof_from:.0f}); the imp was dismissed")
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_crisp_dodge():
    """A move act cuts a long cast at once (Milestone 3c, crisp dodges).

    The server holds a caster still while a cast runs, so without the cut a dodge sent mid-cast
    would wait the whole cast out. Loadb (an orc warlock made level 20) starts `act: use Summon
    Imp` (a 10 s cast), and once its snapshot shows it casting, `act: away from Telda 12` moves it
    12 yd off within a few seconds and the summon is cut.
    """

    import socket
    import time

    from wlblib import server
    from wlblib.common import TEST

    NAME = "crisp_dodge"
    DESCRIPTION = "a move act sent mid-cast cuts the cast and goes at once"
    TEST_CONFIG = "usephys"
    BOT = "Loadb"
    ONLINE = 3

    def run(stack):
        server.console("character level Loadb 20", inst=TEST)
        server.console("llmtest learn Loadb 688", inst=TEST)  # Summon Imp (10 s cast).
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()
        snaps = []   # (monotonic, snapshot)
        errors = {}  # command_id -> [text]

        def pump(until, timeout, what):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                got = until()
                if got:
                    return got
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
                        if err:
                            errors.setdefault(err.get("command_id"), []).append(err.get("text"))
                        continue
                    if msg.get("snapshot"):
                        snaps.append((time.monotonic(), msg["snapshot"]))
            raise AssertionError(f"timed out waiting for {what} (errors: {errors})")

        def latest(after=0.0):
            return next((s for t, s in reversed(snaps)
                         if s.get("bot") == "Loadb" and t >= after), None)

        def telda(snap):
            return snap and next((u for u in snap["base"]["nearby"]["units"]
                                  if u.get("name") == "Telda"), None)

        def casting(snap):
            return (snap or {}).get("base", {}).get("self", {}).get("casting")

        try:
            tpos = pump(lambda: telda(latest()), 20, "Telda in Loadb's sight")["rel"]["position"]
            server.console(f"llmtest move Loadb {tpos['x'] + 3.0} {tpos['y']} {tpos['z'] + 0.5} 0",
                           wait_s=1.0, inst=TEST)
            since_move = time.monotonic()
            start_yd = pump(lambda: telda(latest(since_move)), 10,
                            "Loadb next to Telda")["rel"]["dist_yd"]
            send_cmd(sock, {"bot": "Loadb", "command_id": 41,
                            "act": {"verb": "USE", "name": "Summon Imp"}})
            cast = pump(lambda: casting(latest()), 10, "Loadb casting Summon Imp")
            sent_at = time.monotonic()
            send_cmd(sock, {"bot": "Loadb", "command_id": 42,
                            "act": {"verb": "AWAY", "thing": "NAMED", "name": "Telda",
                                    "yd": 12}})

            def done():
                return (42 in errors) or next(
                    (t for t, s in snaps if s.get("bot") == "Loadb"
                     and s.get("done_act_command_id") == 42), None)
            done_at = pump(done, 8, "Loadb's away act to finish")
            away_errs = errors.get(42, [])
            took = (done_at - sent_at) if isinstance(done_at, float) else None
            after = latest()
            end_yd = (telda(after) or {}).get("rel", {}).get("dist_yd")
            still_casting = casting(after)
        finally:
            sock.close()
        expect(start_yd < 6.0, f"setup: Loadb started {start_yd:.1f} yd from Telda")
        expect("Summon Imp" in str(cast), f"setup: Loadb was casting {cast!r}")
        expect(not away_errs, f"away act failed: {away_errs}")
        expect(took is not None and took < 5.0,
               f"the away act took {took} s with a 10 s cast running (the cast was not cut)")
        expect(end_yd is not None and end_yd >= 10.0, f"Loadb ended {end_yd} yd from Telda")
        expect(not still_casting, f"Loadb still casting after the dodge: {still_casting!r}")
        return (f"mid-cast away act went {start_yd:.0f} -> {end_yd:.0f} yd from Telda in "
                f"{took:.1f} s; the 10 s Summon Imp was cut")
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

CASES = [_case_pull_gate(), _case_crowd_control(), _case_use_physics(), _case_crisp_dodge(), _case_popup_policy(), _case_human_party(), _case_invite(), _case_group(), _case_tank(), _case_popups(), _case_rez_ready(), _case_lootroll(), _case_lfg_proposal()]
