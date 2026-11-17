"""Checks docs/specs/client-events.tsv against the reflected C++ snapshot schema.

Usage: python3 -I tools/audit/check_client_events.py <snapshot_check_bin>
"""

import csv
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
TSV = ROOT / "docs" / "specs" / "client-events.tsv"
CLIENT_EVENTS = ROOT / "out" / "client-ui" / "events_all.txt"

KINDS = {"event", "prompt", "change", "value", "combat_log", "panel", "internal", "out",
         "not_event", "gap"}
FIELD_KINDS = {"change", "value", "combat_log", "panel"}

# Event and prompt kinds the snapshot has that no client event produces.
NOT_FROM_CLIENT = {
    "event.path_failed": "ours: the bot's movement couldn't find a path",
    "event.displaced": "movement packets (knockback, same-map teleport): no client event",
    "event.dbm_warning": "the DBM addon's warnings (addons are emulated server-side)",
    "event.loot_won": "a loot roll result line (CHAT_MSG_LOOT text, split out for the brain)",
    "event.roll": "a /roll result line (CHAT_MSG_SYSTEM text, split out for the brain)",
    "event.quest_complete": "a quest's objectives are done (QUEST_LOG_UPDATE + system text)",
    "event.quest_failed": "a quest failed (system text: 'X failed.')",
    "event.discovered_area": "an explore XP line (CHAT_MSG_SYSTEM / COMBAT_XP_GAIN text)",
    "event.title_earned": "a system text line ('You have earned the title ...')",
    "event.kicked": "SMSG_KICK_REASON: the client disconnects (opcode audit)",
    "event.hearthstone_bound": "SMSG_PLAYERBOUND: system text ('X is now your home.')",
    "event.loot_roll_choice": "others' roll choices: CHAT_MSG_LOOT text, split out",
    "prompt.CONFIRM_ABANDON_QUEST": "a confirmation the bot's own action raises (no event)",
    "prompt.CONFIRM_RELEASE_SPIRIT": "the release-spirit popup after PLAYER_DEAD",
    "prompt.CONFIRM_PETITION_SIGN": "PETITION_SHOW of someone else's charter",
    "prompt.CONFIRM_LEAVE_BATTLEGROUND": "a confirmation the bot's own action raises",
    "prompt.CONFIRM_GUILD_LEADER": "a confirmation the bot's own action raises",
    "prompt.CONFIRM_REPLACE_GEM": "SOCKET_INFO_UPDATE confirmation on the bot's own action",
    "prompt.BATTLEGROUND_READY": "UPDATE_BATTLEFIELD_STATUS with status 'confirm'",
}


def main():
    raw = subprocess.check_output([sys.argv[1], "--schema-json"], text=True)
    schema = json.loads(raw)
    events = set(schema["events"])
    prompts = set(schema["prompts"])
    fields = set(schema["fields"])
    errors = []
    with open(TSV) as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    used = set()
    for r in rows:
        event, kind, maps_to = r["event"], r["kind"], r["maps_to"]
        if kind not in KINDS:
            errors.append(f"{event}: kind {kind!r} (classify it)")
            continue
        if kind == "event" and maps_to not in events:
            errors.append(f"{event}: {maps_to!r} is not an Event kind")
        elif kind == "prompt" and maps_to not in prompts:
            errors.append(f"{event}: {maps_to!r} is not a PromptEvent kind")
        elif kind in FIELD_KINDS and maps_to not in fields:
            errors.append(f"{event}: {maps_to!r} is not a snapshot field")
        elif kind == "gap" and maps_to not in ("event", "prompt", "field"):
            errors.append(f"{event}: a gap says what it needs: event, prompt or field")
        elif kind in ("internal", "out", "not_event") and maps_to:
            errors.append(f"{event}: {kind} maps to nothing")
        used.add(maps_to)
    for name in sorted((events | prompts) - used - set(NOT_FROM_CLIENT)):
        errors.append(f"{name}: no client event produces it; map one, or add it to "
                      "NOT_FROM_CLIENT with the reason")
    for name in sorted(set(NOT_FROM_CLIENT) - events - prompts):
        errors.append(f"NOT_FROM_CLIENT lists {name}, which the proto doesn't have")
    if CLIENT_EVENTS.exists():
        client = set(CLIENT_EVENTS.read_text().split())
        listed = {r["event"] for r in rows}
        for e in sorted(client - listed):
            errors.append(f"{e}: a client event missing from the table")
        for e in sorted(listed - client):
            errors.append(f"{e}: in the table but not a client event")
    gaps = sum(r["kind"] == "gap" for r in rows)
    for e in errors:
        print("client-events.tsv:", e)
    print(f"client events: {len(rows)} classified, {gaps} gaps, {len(errors)} errors")
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
