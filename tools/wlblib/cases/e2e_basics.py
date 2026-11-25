"""Grouped e2e cases: keep, choose, walk, rest, talk, panels, bind, death."""

import types
from wlblib import wire
from wlblib.testing import expect, find_option, send_cmd

def _case_keep():
    """Keep while walking: the walk carries on and arrives (review of f40b0b6, finding 1).

    A Choose of "Keep doing what I'm doing" must leave the body's current step alone. It used to
    reset the walk: on map 1 the walk then failed ("you changed map"), on map 0 the bot walked to
    (0, 0, 0).
    """

    import math

    from wlblib.testing import expect

    NAME = "keep"
    DESCRIPTION = "Explore, then Keep: the walk carries on and is reported done on arrival"
    TEST_CONFIG = "choose"  # One level-1 priest in the Valley of Trials (map 1), grinding off.
    BOT = "Loada"
    ARRIVED_WITHIN_S = 90
    MOVED_AT_LEAST_YD = 20


    def run(stack):
        t = stack.play(["Explore", "Keep"], seconds=ARRIVED_WITHIN_S + 30,
                       tail_s=ARRIVED_WITHIN_S)
        expect(not t["closed"], "the module closed the connection")
        expect(not t["never_offered"], f"never offered: {t['never_offered']}")
        explore, keep = t["commands"]
        failed = [e["event"]["error"] for e in t["events"]
                  if e["event"].get("error", {}).get("command_id") in (explore["command_id"],
                                                                         keep["command_id"])]
        expect(not failed, f"the walk failed after Keep: {failed}")
        before = next(s for s in t["snapshots"] if s["seq"] == explore["seq"])
        done = next((s for s in t["snapshots"] if s["t"] > keep["t"]
                     and s["done_command_id"] == explore["command_id"]), None)
        expect(done, f"the walk wasn't reported done within {ARRIVED_WITHIN_S} s after Keep")
        moved = math.dist(before["pos"], done["pos"])
        expect(moved >= MOVED_AT_LEAST_YD, f"done after moving only {moved:.0f} yd")
        return (f"Keep {keep['t'] - explore['t']:.1f} s into the walk; arrived "
                f"{done['t'] - explore['t']:.1f} s after Explore, {moved:.0f} yd walked")
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_choose():
    """A bot told to Attack a boar fights it; told to Loot, it loots the corpse.

    The whole chain of step M1 (docs/specs/harness.md): the module offers the options, a Choose
    names one of them in a snapshot, the bot's own body (playerbots) carries it out, and the world
    shows it. The brain here is the scripted one of `tools/wlb snap fake` (tools/wlblib/wire.py).
    """

    import re

    from wlblib.testing import expect

    NAME = "choose"
    DESCRIPTION = "Attack a boar, it fights; Loot the corpse, it is looted (one Choose each)"
    TEST_CONFIG = "choose"  # config/tests/choose.*.overrides: one level-1 priest next to a boar.
    BOT = "Loada"
    # age_at_apply here is the plumbing only: the scripted brain answers at once.
    MAX_AGE_AT_APPLY_MS = 100
    # After a Choose, how long the world may take to show it (walking to the boar, a fight).
    FIGHT_WITHIN_S = 20
    LOOTED_WITHIN_S = 30

    CHOSE = re.compile(r"chose (\d+) of snapshot (\d+) \((.*?)\): (.*?), age_at_apply=(-?\d+) ms")


    def run(stack):
        t = stack.play(["Attack", "Loot"])
        expect(not t["closed"], "the module closed the connection")
        expect(not t["never_offered"],
               f"never offered: {', '.join(t['never_offered'])} "
               f"(options seen: {sorted({o for s in t['snapshots'] for o in s['options']})})")
        attack, loot = t["commands"]
        snapshots = t["snapshots"]

        # No Choose failed: a failed one comes back as an error event naming its command.
        ids = {attack["command_id"], loot["command_id"]}
        failed = [e["event"]["error"] for e in t["events"]
                  if e["event"].get("error", {}).get("command_id") in ids]
        expect(not failed, f"a choice failed: {failed}")

        # Attack: the bot is in combat soon after.
        fight = next((s for s in snapshots if s["t"] > attack["t"] and s["in_combat"]), None)
        expect(fight and fight["t"] - attack["t"] <= FIGHT_WITHIN_S,
               f"the bot wasn't in combat within {FIGHT_WITHIN_S} s of Attack")

        # Loot: the corpse is looted, so the Loot option goes away.
        cleared = next((s for s in snapshots if s["t"] > loot["t"]
                        and not any(o.startswith("Loot") for o in s["options"])), None)
        expect(cleared and cleared["t"] - loot["t"] <= LOOTED_WITHIN_S,
               f"the corpse still offered Loot {LOOTED_WITHIN_S} s after Loot")

        # The module's own record of each choice: accepted, and how old the picture was.
        chose = CHOSE.findall(stack.server_log())
        expect(len(chose) == 2, f"expected 2 'chose' lines in the server log, found {len(chose)}")
        for option, seq, what, result, age_ms in chose:
            expect(result == "ok", f"the module refused '{what}': {result}")
            expect(int(age_ms) <= MAX_AGE_AT_APPLY_MS,
                   f"age_at_apply {age_ms} ms for '{what}' (limit {MAX_AGE_AT_APPLY_MS} ms)")
        ages = "/".join(c[4] for c in chose)
        return (f"in combat {fight['t'] - attack['t']:.1f} s after Attack, looted "
                f"{cleared['t'] - loot['t']:.1f} s after Loot, age_at_apply {ages} ms")
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_walk():
    """A bot told to Explore walks to the spot and the module reports the step done.

    The stepped mmap walk of docs/specs/options.md: Explore is offered from the module's facts, a
    Choose starts the walk, the module steps it ~70 yd at a time and sets `done_command_id` on
    arrival. Talk and Rest have no live case yet (docs/TODO.md, "Loose ends").
    """

    import math

    from wlblib.testing import expect

    NAME = "walk"
    DESCRIPTION = "Explore: the bot walks to the spot and the step is reported done"
    TEST_CONFIG = "choose"  # One level-1 priest in the Valley of Trials, its own grinding off.
    BOT = "Loada"
    # Spots are 25 yd to a few hundred yd away; a level-1 walks ~7 yd/s.
    ARRIVED_WITHIN_S = 90
    MOVED_AT_LEAST_YD = 20


    def run(stack):
        t = stack.play(["Explore"], seconds=ARRIVED_WITHIN_S + 30, tail_s=ARRIVED_WITHIN_S)
        expect(not t["closed"], "the module closed the connection")
        expect(not t["never_offered"],
               f"Explore never offered (options seen: "
               f"{sorted({o for s in t['snapshots'] for o in s['options']})})")
        (explore,) = t["commands"]
        failed = [e["event"]["error"] for e in t["events"]
                  if e["event"].get("error", {}).get("command_id") == explore["command_id"]]
        expect(not failed, f"the walk failed: {failed}")

        before = next(s for s in t["snapshots"] if s["seq"] == explore["seq"])
        done = next((s for s in t["snapshots"]
                     if s["done_command_id"] == explore["command_id"]), None)
        expect(done, f"no done signal within {ARRIVED_WITHIN_S} s of '{explore['text']}'")
        moved = math.dist(before["pos"], done["pos"])
        expect(moved >= MOVED_AT_LEAST_YD, f"done after moving only {moved:.0f} yd")
        return (f"'{explore['text']}': done {done['t'] - explore['t']:.1f} s later, "
                f"{moved:.0f} yd walked")
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_rest():
    """A bot hurt by a fight, told to Rest, eats or drinks until full and the step is done.

    docs/specs/options.md, "Rest": offered out of combat below full health or mana; the body uses
    the food and water in its bags (the fixture carries bread and water; playerbots' eat / drink
    actions, no free food) or sits. The fight comes first: the same scene as `e2e choose`. It must
    really eat or drink: a Food or Drink aura shows while resting (review of f40b0b6, finding 15:
    it passed by sitting).
    """

    from wlblib.testing import expect

    NAME = "rest"
    DESCRIPTION = "after a fight, Rest: health and mana back to full, step reported done"
    TEST_CONFIG = "choose"  # One level-1 priest next to a boar.
    BOT = "Loada"
    REST_WITHIN_S = 60
    # The boar may still be respawning when `e2e choose` ran first on the same stack (about 2 min).
    RUN_S = 300


    def run(stack):
        t = stack.play(["Attack", "Rest"], seconds=RUN_S, tail_s=REST_WITHIN_S)
        expect(not t["closed"], "the module closed the connection")
        expect(not t["never_offered"],
               f"never offered: {', '.join(t['never_offered'])} (the fight left the bot at full "
               f"health and mana?)")
        rest = t["commands"][1]
        failed = [e["event"]["error"] for e in t["events"]
                  if e["event"].get("error", {}).get("command_id") == rest["command_id"]]
        expect(not failed, f"Rest failed: {failed}")

        start = next(s for s in t["snapshots"] if s["seq"] == rest["seq"])
        done = next((s for s in t["snapshots"] if s["done_command_id"] == rest["command_id"]), None)
        last = t["snapshots"][-1]
        expect(done and done["t"] - rest["t"] <= REST_WITHIN_S,
               f"no done signal within {REST_WITHIN_S} s of Rest (watched "
               f"{last['t'] - rest['t']:.0f} s; last {last['health']}/{last['health_max']} hp, "
               f"{last['power']}/{last['power_max']} mana)")
        expect(done["health"] == done["health_max"] and done["power"] == done["power_max"],
               f"done at {done['health']}/{done['health_max']} health, "
               f"{done['power']}/{done['power_max']} mana")
        resting = [s for s in t["snapshots"] if rest["t"] <= s["t"] <= done["t"]]
        ate = any("Food" in s["auras"] for s in resting)
        drank = any("Drink" in s["auras"] for s in resting)
        expect(ate or drank, "rested without eating or drinking (no Food or Drink aura)")
        return (f"{start['health']}/{start['health_max']} hp, {start['power']}/{start['power_max']} "
                f"mana -> full ({'ate' if ate else ''}{' and ' if ate and drank else ''}"
                f"{'drank' if drank else ''}), done {done['t'] - rest['t']:.1f} s after Rest")
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_talk():
    """A bot approaches a quest giver, then sees and chooses an explicit per-quest option.

    Medium 2 ("Talk to" split into real brain choices):
      1. Starting 6 yd from Kaltunk (just outside 5 yd talk range), the brain chooses
         `Talk to Kaltunk` to walk up; C++ must NOT auto-accept quests on approach.
      2. Once in talk range, the brain is offered `Accept quest "Your Place In The
         World" from Kaltunk`, chooses it, and receives the `info` event +
         `done_command_id`.
    """

    from wlblib.testing import expect

    NAME = "talk"
    DESCRIPTION = "Walk to Kaltunk, then explicitly accept his quest; info event and done signal"
    TEST_CONFIG = "talk"  # A fresh level-1 priest 6 yd in front of Kaltunk.
    BOT = "Loada"
    DONE_WITHIN_S = 20


    def run(stack):
        t = stack.play(
            ["Talk to Kaltunk", 'Accept quest "Your Place In The World" from Kaltunk'],
            seconds=60,
            tail_s=DONE_WITHIN_S,
        )
        expect(not t["closed"], "the module closed the connection")
        expect(
            not t["never_offered"],
            f"expected option never offered ({t['never_offered']}; options seen: "
            f"{sorted({o for s in t['snapshots'] for o in s['options']})})",
        )
        expect(len(t["commands"]) == 2, f"expected 2 commands, got {t['commands']}")
        talk, accept = t["commands"]
        failed = [
            e["event"]["error"]
            for e in t["events"]
            if e["event"].get("error", {}).get("command_id")
            in (talk["command_id"], accept["command_id"])
        ]
        expect(not failed, f"command failed: {failed}")

        infos = [
            e["event"]["info"]["text"]
            for e in t["events"]
            if e["t"] >= accept["t"] and "info" in e["event"]
        ]
        said = next(
            (i for i in infos if "accepted" in i and "Your Place In The World" in i),
            None,
        )
        expect(said, f"no 'accepted Your Place In The World' info event (info events: {infos})")
        done = next(
            (s for s in t["snapshots"] if s["done_command_id"] == accept["command_id"]),
            None,
        )
        expect(
            done and done["t"] - accept["t"] <= DONE_WITHIN_S,
            f"no done signal within {DONE_WITHIN_S} s of quest accept",
        )
        return f"{said} (done {done['t'] - accept['t']:.1f} s later)"
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_panels():
    """Open bags, point at items, close bags: the panel and tooltips show and go.

    docs/specs/options.md: `open` / `close` / `hover` commands; the module fills `ext.bags` only while
    open and the tooltip for a few snapshots; item "Use:" text comes from Spell.dbc with its values
    filled in as the client does: the hearthstone's home ($z), the bread's "61 health over 18 sec"
    ($o1 of a regen aura: per 5 s, floating point) and the potion's "70 to 90" (a range, never a
    roll): review of f40b0b6, finding 16.
    """

    from wlblib.testing import expect

    NAME = "panels"
    DESCRIPTION = "open bags, hover the hearthstone (tooltip from Spell.dbc), close bags"
    TEST_CONFIG = "talk"  # One fresh level-1 priest standing still at the Valley of Trials.
    BOT = "Loada"


    def run(stack):
        t = stack.play(["open:bags", "hover:Hearthstone", "hover:Tough Hunk of Bread",
                        "hover:Minor Healing Potion", "close:bags"], seconds=50, tail_s=6)
        expect(not t["closed"], "the module closed the connection")
        expect(not t["never_offered"], f"never sent: {', '.join(t['never_offered'])} (no "
               "Hearthstone in the open bags?)")
        open_cmd, hover_cmd, bread_cmd, potion_cmd, close_cmd = t["commands"]
        errors = [e["event"]["error"]["text"] for e in t["events"] if "error" in e["event"]]
        expect(not errors, f"the module refused: {errors}")

        shown = next((s for s in t["snapshots"]
                      if s["t"] > open_cmd["t"] and "BAGS" in s["open_panels"]), None)
        expect(shown, "no snapshot showed the bags after `open`")
        expect("Hearthstone" in shown["bag_items"], f"bags without a Hearthstone: {shown['bag_items']}")

        def use_line(cmd, until, start):
            tip = next((s["tooltips"][0] for s in t["snapshots"]
                        if cmd["t"] < s["t"] < until and s["tooltips"]
                        and any(line.startswith(start) for line in s["tooltips"][0])), None)
            expect(tip, f"no tooltip with '{start}' after `{cmd['text']}`")
            return next(line for line in tip if line.startswith(start))

        use = use_line(hover_cmd, bread_cmd["t"] + 2, "Use: Returns you to")
        expect("$z" not in use, f"the hearthstone's home not filled in: {use}")
        bread = use_line(bread_cmd, potion_cmd["t"] + 2, "Use: Restores")
        expect(bread.startswith("Use: Restores 61 health over 18 sec"),
               f"the bread's tooltip: {bread!r}, expected 'Restores 61 health over 18 sec'")
        potion = use_line(potion_cmd, close_cmd["t"] + 2, "Use: Restores")
        expect(potion.startswith("Use: Restores 70 to 90 health"),
               f"the potion's tooltip: {potion!r}, expected 'Restores 70 to 90 health'")

        gone = next((s for s in t["snapshots"]
                     if s["t"] > close_cmd["t"] and "BAGS" not in s["open_panels"]
                     and not s["bag_items"]), None)
        expect(gone, "the bags still showed after `close`")
        return (f"{len(shown['bag_items'])} items in the bags; '{use}'; '{bread}'; '{potion}'; closed "
                f"{gone['t'] - close_cmd['t']:.1f} s after close")
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_bind():
    """Bind hearthstone option at an innkeeper (F4, docs/specs/options.md).

      1. Record Loada's starting spot, then teleport her to Innkeeper Grosk in
         Razor Hill (Durotar, map 1).
      2. Verify `Bind hearthstone at Innkeeper Grosk (Razor Hill)` is offered.
      3. Choose it: verify `"bound hearthstone at Innkeeper Grosk"` info event,
         no error, and the bind option disappears once bound here.
      4. Open the bags panel and verify `bags.hearthstone.bind == "Razor Hill"`.
      5. Close bags and teleport Loada back to her starting spot so sibling cases
         sharing the `talk` stack are unaffected.
    """

    import socket
    import sys
    import time

    from wlblib import server
    from wlblib.common import ROOT, TEST
    from wlblib.testing import expect


    NAME = "bind"
    DESCRIPTION = "Bind hearthstone option at an innkeeper binds home via the server bind spell"
    TEST_CONFIG = "talk"  # Fresh level-1 Troll Priest (starts bound in Valley of Trials).
    BOT = "Loada"
    # Innkeeper Grosk (entry 6928) in Razor Hill, Durotar (map 1).
    RAZOR_HILL_INN = (1, 340.36, -4686.29, 16.54)




    def run(stack):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()

        cmd_id = 0
        origin = None
        teleported = False
        bind_cmd_id = None
        bind_option_text = None
        bind_info = None
        bind_gone = False
        bags_opened = False
        hearth_bind = None
        errors = []

        deadline = time.time() + 45.0
        try:
            while time.time() < deadline and not (bind_info and bind_gone and hearth_bind == "Razor Hill"):
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
                        err = ev.get("error")
                        if err and err.get("command_id") == bind_cmd_id:
                            errors.append(err.get("text"))
                        info = ev.get("info")
                        if info and "bound hearthstone at Innkeeper Grosk" in info.get("text", ""):
                            bind_info = info["text"]
                        continue
                    snap = msg.get("snapshot")
                    if not snap or snap.get("bot") != BOT:
                        continue
                    if not teleported:
                        where = snap.get("base", {}).get("where", {})
                        pos = where.get("position") or {}
                        if pos:
                            origin = (pos.get("x", 0.0), pos.get("y", 0.0), pos.get("z", 0.0),
                                      where.get("facing", 0.0))
                        m, x, y, z = RAZOR_HILL_INN
                        server.console(f"llmtest tele {BOT} {m} {x} {y} {z} 0", inst=TEST)
                        teleported = True
                        continue
                    seq = snap.get("seq", 0)
                    options = snap.get("options", [])
                    bind_idx = next(
                        (i for i, o in enumerate(options, 1)
                         if o.get("guid", "").startswith("npc_bind:")),
                        None,
                    )
                    if bind_cmd_id is None and bind_idx is not None:
                        bind_option_text = options[bind_idx - 1].get("text", "")
                        cmd_id += 1
                        bind_cmd_id = cmd_id
                        send_cmd(sock, {
                            "bot": BOT,
                            "command_id": cmd_id,
                            "choose": {"option": bind_idx, "seq": seq},
                        })
                    elif bind_cmd_id is not None:
                        if bind_idx is None:
                            bind_gone = True
                        if not bags_opened and bind_info:
                            cmd_id += 1
                            send_cmd(sock, {
                                "bot": BOT,
                                "command_id": cmd_id,
                                "open": {"panel": "BAGS"},
                            })
                            bags_opened = True
                        bags = snap.get("ext", {}).get("bags") or {}
                        hs = bags.get("hearthstone") or {}
                        if hs.get("bind"):
                            hearth_bind = hs["bind"]
        finally:
            if bags_opened:
                try:
                    cmd_id += 1
                    send_cmd(sock, {
                        "bot": BOT,
                        "command_id": cmd_id,
                        "close": {"panel": "BAGS"},
                    })
                except OSError:
                    pass
            sock.close()
            if origin is not None:
                ox, oy, oz, oo = origin
                server.console(f"llmtest tele {BOT} 1 {ox:.2f} {oy:.2f} {oz:.2f} {oo:.2f}",
                               wait_s=0.5, inst=TEST)

        expect(bind_cmd_id is not None, "Loada was never offered `Bind hearthstone` at Innkeeper Grosk")
        expect("Innkeeper Grosk" in (bind_option_text or "") and "Razor Hill" in (bind_option_text or ""),
               f"unexpected bind option text: {bind_option_text!r}")
        expect(not errors, f"Bind hearthstone returned error: {errors}")
        expect(bind_info is not None, "no 'bound hearthstone at Innkeeper Grosk' info event")
        expect(bind_gone, "Bind hearthstone option stayed after binding at Razor Hill")
        expect(hearth_bind == "Razor Hill",
               f"expected hearthstone bind 'Razor Hill' in bags panel, got {hearth_bind!r}")
        return f"offered '{bind_option_text}'; {bind_info}; bags hearthstone bind={hearth_bind}"
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_death():
    """Open-world death -> Release spirit -> ghost corpse run -> respawn alive (F4).

      1. Kill Loada in Valley of Trials (`llmtest kill Loada`).
      2. Snapshot shows `DEAD_BODY` and offers `Release spirit`; choosing it
         succeeds and moves Loada to `GHOST` at the graveyard.
      3. Playerbots' dead strategy walks the ghost back to her corpse (no teleport
         cheat) and revives her: snapshot returns to `ALIVE` with health > 0.
    """

    import socket
    import sys
    import time

    from wlblib import server
    from wlblib.common import ROOT, TEST
    from wlblib.testing import expect


    NAME = "death"
    DESCRIPTION = "Open-world death, Release spirit, ghost corpse run, and respawn alive"
    TEST_CONFIG = "choose"  # Fresh level-1 Troll Priest in Valley of Trials.
    BOT = "Loada"




    def run(stack):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(str(TEST.run / "brain.sock"))
        reader = wire.FrameReader()

        cmd_id = 0
        release_cmd_id = None
        phase = "wait_alive"  # wait_alive -> dead -> ghost -> alive
        saw_dead_body = False
        saw_ghost = False
        revived_alive = False
        ghost_at = None
        revived_at = None
        errors = []

        deadline = time.time() + 90.0
        try:
            while time.time() < deadline and phase != "alive":
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
                        err = ev.get("error")
                        if err and err.get("command_id") == release_cmd_id:
                            errors.append(err.get("text"))
                        continue
                    snap = msg.get("snapshot")
                    if not snap or snap.get("bot") != BOT:
                        continue
                    seq = snap.get("seq", 0)
                    options = snap.get("options", [])
                    self_info = snap.get("base", {}).get("self", {})
                    state = self_info.get("state", "")
                    health = self_info.get("health", 0)

                    if phase == "wait_alive" and state == "ALIVE":
                        server.console(f"llmtest kill {BOT}", inst=TEST)
                        phase = "dead"
                    elif phase == "dead" and state == "DEAD_BODY":
                        saw_dead_body = True
                        rel_idx = next(
                            (i for i, o in enumerate(options, 1)
                             if o.get("guid") == "rez:release"),
                            None,
                        )
                        if rel_idx is not None and release_cmd_id is None:
                            cmd_id += 1
                            release_cmd_id = cmd_id
                            send_cmd(sock, {
                                "bot": BOT,
                                "command_id": cmd_id,
                                "choose": {"option": rel_idx, "seq": seq},
                            })
                    elif phase == "dead" and release_cmd_id is not None and state == "GHOST":
                        saw_ghost = True
                        ghost_at = time.time()
                        phase = "ghost"
                    elif phase == "ghost" and state == "ALIVE" and health > 0:
                        revived_alive = True
                        revived_at = time.time()
                        phase = "alive"
        finally:
            sock.close()

        expect(saw_dead_body, "Loada never entered DEAD_BODY after llmtest kill")
        expect(release_cmd_id is not None, "dead Loada was never offered `Release spirit`")
        expect(not errors, f"Release spirit returned error: {errors}")
        expect(saw_ghost, "Loada never transitioned to GHOST after Release spirit")
        expect(revived_alive, "ghost Loada did not corpse-run and respawn ALIVE within timeout")
        return f"DEAD_BODY -> Release spirit -> GHOST -> ALIVE in {revived_at - ghost_at:.1f} s"
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_create_bots():
    """LlmBrain.CreateBots creates a missing bot once and never resets it (T1 play preset).

    `createbots` test config: Loadca=new:dwarf:warrior. After the first start the character
    exists as a dwarf warrior and logs in; a restart logs it in again without creating it anew.
    """

    import subprocess
    import time

    from wlblib.common import OUT
    from wlblib.testing import expect

    NAME = "create_bots"
    DESCRIPTION = "CreateBots makes a missing dwarf warrior, logs it in, and keeps it on restart"
    TEST_CONFIG = "createbots"
    BOT = "Loadca"

    def _character():
        out = subprocess.run(
            ["mysql", f"--defaults-file={OUT / 'server' / 'etc' / 'mysql.cnf'}", "-uroot", "-N",
             "-e", "select race, class, guid from acore_test_characters.characters "
                   "where name = 'Loadca'"],
            capture_output=True, text=True, check=True).stdout.split()
        return out

    def _wait_logged_in(stack, what):
        from wlblib import server
        from wlblib.common import TEST
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if "Loadca" in server.online_characters(TEST):
                return
            time.sleep(0.5)
        expect(False, f"timed out waiting for {what}")

    def run(stack):
        _wait_logged_in(stack, "Loadca online")
        first = _character()
        expect(first[:2] == ["3", "1"], f"Loadca is not a dwarf (3) warrior (1): {first}")
        stack.switch(TEST_CONFIG, BOT)
        _wait_logged_in(stack, "Loadca online after a restart")
        log = stack.server_log()
        expect("created Loadca" not in log, "a restart created Loadca again")
        expect(_character() == first, f"a restart changed Loadca: {first} -> {_character()}")
        return f"Loadca (guid {first[2]}) created as a dwarf warrior; a restart kept it"
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

def _case_self_rez():
    """A soulstoned bot that dies is offered to resurrect itself, and gets up (Now #6).

    `choose` test config (one level-1 priest). Loada gets Soulstone Resurrection, is killed, is
    offered `Use <spell> (resurrect yourself)`; choosing it brings her back alive.
    """

    import time

    from wlblib import server
    from wlblib.common import TEST
    from wlblib.testing import expect

    NAME = "self_rez"
    DESCRIPTION = "Soulstone up: dead Loada is offered self-resurrection and gets up"
    TEST_CONFIG = "choose"
    BOT = "Loada"

    def run(stack):
        server.console("llmtest aura Loada 20707", inst=TEST)  # Soulstone Resurrection.
        server.console("llmtest kill Loada", inst=TEST)
        t = stack.play(["Use Soulstone", "Keep"], seconds=40, tail_s=10)
        last = t["snapshots"][-1] if t["snapshots"] else {}
        expect(not t["never_offered"],
               f"never offered: {t['never_offered']}; last screen ({len(t['snapshots'])} "
               f"snapshots): health {last.get('health')}, options {last.get('options')}")
        use = t["commands"][0]
        alive_after = [s for s in t["snapshots"] if s["t"] > use["t"] and s["health"] > 0]
        expect(alive_after, "Loada wasn't alive after using the soulstone")
        return f"self-resurrected {alive_after[0]['t'] - use['t']:.1f} s after choosing it"
    return types.SimpleNamespace(NAME=NAME, DESCRIPTION=DESCRIPTION, TEST_CONFIG=TEST_CONFIG, BOT=BOT, ONLINE=locals().get('ONLINE', 1), run=run)

CASES = [_case_self_rez(), _case_create_bots(), _case_keep(), _case_choose(), _case_walk(), _case_rest(), _case_talk(), _case_panels(), _case_bind(), _case_death()]
