"""Checks docs/specs/client-events.tsv against the snapshot proto.

The table classifies every event the 3.3.5a client knows (the client-event
audit, docs/specs/coverage-client-events.md). This keeps it honest:
1. every row is classified (no `review`), with a known kind;
2. every `maps_to` names something that exists in the proto: an Event kind,
   a PromptEvent kind, or a snapshot field path;
3. every Event kind and PromptEvent kind in the proto is produced by some
   client event, or is listed below as not coming from one (with the reason),
   so a new event or prompt can't be added without deciding where it comes from;
4. when the extracted client data is present (tools/extract-client-ui.sh), the
   table lists exactly the client's events.

Usage: python3 -I tools/audit/check_client_events.py <generated_proto_dir>
"""

import csv
import pathlib
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


def field_paths(descriptor, prefix, depth=4):
    paths = set()
    for f in descriptor.fields:
        path = prefix + f.name
        paths.add(path)
        sub = f.message_type
        if sub is not None and depth > 0 and not sub.GetOptions().map_entry:
            paths |= field_paths(sub, path + ".", depth - 1)
    return paths


def main():
    sys.path.insert(0, sys.argv[1])
    from proto import snapshot_pb2 as pb  # noqa: E402

    # `prompt` wraps the PromptEvent kinds, which are checked one by one.
    events = {"event." + f.name for f in pb.Event.DESCRIPTOR.fields if f.name != "prompt"}
    prompts = {"prompt." + v.name
               for v in pb.PromptEvent.DESCRIPTOR.enum_types_by_name["Kind"].values
               if v.number != 0}
    fields = field_paths(pb.Snapshot.DESCRIPTOR, "")
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
