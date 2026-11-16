# Coverage: client events (the full list of triggers)

The brain is woken by triggers: something happened that the bot's current plan didn't account
for. For that to work, the list of things that can happen must be **complete**. This audit
builds it from the 3.3.5a client itself: every event the client can fire is something a player
can be told about, and nothing else is.

## Sources

`tools/extract-client-ui.sh` (needs the client and a server build) extracts, into the
untracked `out/client-ui/`:

- **The default UI code** (FrameXML, GlueXML, the Blizzard_* addons: 463 files) from the
  client's MPQs, with AzerothCore's libmpq.
- **Every event name**: the event tables in `Wow.exe` (688 names), plus every event the UI
  registers or compares against (659, including the chat events it builds from
  `ChatTypeGroup`): **713 together**.

## The table

`docs/specs/client-events.tsv` (tracked) classifies all 713. Drafted by rules
(`tools/audit/classify_client_events.py`), then reviewed. Columns: event, kind, `maps_to` (where
it lands in the snapshot), `client_alert` (how the client alerts on it, from the UI code), note.

| Kind | Count | Meaning | How it can wake the brain |
|---|---|---|---|
| event | 109 | a discrete happening | an Event in `events` |
| prompt | 38 | a popup that needs an answer | a PromptEvent |
| change | 206 | a discrete change of state (in combat, lost control, target, auras, party) | the field changed |
| value | 24 | a continuous value (health, power, cooldowns) | a watch the brain sets ("health under 40%") |
| combat_log | 49 | combat log sub-events (hits, heals, auras, casts around the player) | `ext.combat`; hits on me |
| panel | 92 | contents of a window the bot opened itself (vendor, trainer, mail) | none: the bot's own action |
| internal | 50 | client plumbing (loading, settings, key bindings) | none |
| out | 124 | out of scope (Battle.net, voice chat, GM tools, login screens) | none |
| not_event | 21 | names the extraction picked up that aren't events | none |
| gap | 0 | visible to a player, no place in the snapshot yet (7 found, all filled) | below |

## Client alerts

`tools/audit/client_alerts.py` reads the UI code and records, per event, what the client does
to get the player's attention: a sound, a popup, a screen or chat-tab flash (including the
low-health flash), a raid notice, an error line, an alert frame (achievement, dungeon
complete), zone text, or combat feedback (damage shown on a portrait). It follows calls into
helper functions. Every event with such an alert has been checked against its kind; that
cross-check found most of the gaps below.

Notably, **the 3.3.5a UI plays no sound when you're attacked**: a player notices from the
combat feedback on their portrait (`UNIT_COMBAT`), the entering-combat flash
(`PLAYER_REGEN_DISABLED`), and the combat log. All three are triggers here.

Limits: the scan is static (Lua isn't parsed) and can't see alerts the client makes in C; it
also counts any frame shown by an event (`shows_frame`), which is noisy (most are windows
updating). The ready check is one C-side case (`ShowReadyCheck` shows its own frame).

## Gaps found (all filled)

From the client events, filled with new prompt and event kinds:

| Event | Needs | |
|---|---|---|
| `AREA_SPIRIT_HEALER_IN_RANGE` | prompt `SPIRIT_HEALER_WAVE` | battleground spirit healer: "resurrect in N s?" |
| `CORPSE_IN_RANGE` | prompt `RECLAIM_CORPSE` | "resurrect now?" at your corpse (or an `ext.death` option) |
| `CHANNEL_INVITE_REQUEST`, `CHANNEL_PASSWORD_REQUEST` | prompts `CHANNEL_INVITE`, `CHANNEL_PASSWORD` | chat channel invite / password |
| `PET_RENAMEABLE` | prompt `PET_NAME` | name your new pet |
| `MINIMAP_PING` | event `minimap_ping` | someone pinged the minimap: who and where |
| `CALENDAR_EVENT_ALARM` | event `calendar_alarm` | a calendar event starts soon |

From the opcode audit's 66 "state" opcodes (`coverage-opcodes.md`), which change state without
any client event (a player only notices by looking):

| Opcodes | Needs | |
|---|---|---|
| `MSG_MOVE_KNOCK_BACK`, `SMSG_MOVE_KNOCK_BACK`, `MSG_MOVE_TELEPORT` | event `displaced` | **displaced**: knocked back, pulled, or teleported on the same map |
| `SMSG_UPDATE_OBJECT`, `SMSG_DESTROY_OBJECT` | watch `appears` / `gone` (`triggers.md`) | units and objects **coming into or leaving view**: the brain's watches must be able to say "a unit matching X appears" |
| `SMSG_TRANSFER_ABORTED`, `SMSG_LFG_TELEPORT_DENIED` | event `error` (`snapshot.md` §8) | a failed teleport, as an `error` event |

The rest of the 66 already have a trigger (health and power: value; control, mirror timers,
world states: change) or are movement and physics details (positions, which the brain can
watch; gravity, turn rates: internal).

## Enforced

`tools/validate-snapshots.sh` runs `tools/audit/check_client_events.py`, which fails when:
a row isn't classified; a `maps_to` names something the proto doesn't have; an Event or
PromptEvent kind in the proto is produced by no client event and isn't listed as coming from
elsewhere (with the reason: our own path failures, DBM warnings, lines split out of chat text);
or, when the client data is extracted, the table and the client's event list differ.

## Played scenarios (to run once triggers are built; `triggers.md`)

Fixture tests on the live server, each checking that the bot's brain is woken, with what, and
how fast: a test character attacks Testpriest mid-quest (gank); a mob stuns it; a whisper; a
group invite; a ready check; a knockback; health drops under a watched line; an Alliance player
comes into view with a watch set; the bot dies; a Dungeon Finder proposal.

## Re-running

```
tools/extract-client-ui.sh                       # client data -> out/client-ui
python3 -I tools/audit/client_alerts.py          # alerts per event
python3 -I tools/audit/classify_client_events.py # adds new events to the table, as `review`
tools/validate-snapshots.sh                      # checks the table
```
