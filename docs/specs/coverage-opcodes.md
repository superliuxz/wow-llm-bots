# Snapshot coverage: server-to-client opcodes

Audit of `snapshot.md` (rev 6) against every `SMSG_*` and `MSG_*` opcode in AzerothCore's
`Opcodes.h` (688). A packet the server sends to a player is something the client can show;
each one is either a snapshot field or event, internal to the protocol, or out of scope.

Method: rules by prefix and keyword classified 628 opcodes; the 60 left over were reviewed
by hand; then every "mapped" opcode for a feature was re-checked against an actual field in
the spec. Working files (untracked): `out/audit/opcodes_classified.tsv`.

## Summary

| Class | Count | Meaning |
|---|---|---|
| internal | 194 | protocol, caches, client config, or folded into the 1 s state read |
| state | 66 | read from server state each second, not from the packet |
| mapped, field exists | ~300 | a spec field or event carries it |
| mapped, **no field yet** | ~70 | the feature is visible but the spec lacks it (below) |
| hand-reviewed | 60 | below |

## Mapped, but no field yet

| Feature | Opcodes (examples) | Fix |
|---|---|---|
| Achievements | SMSG_ACHIEVEMENT_EARNED, SMSG_ALL_ACHIEVEMENT_DATA, SMSG_CRITERIA_UPDATE, SMSG_SERVER_FIRST_ACHIEVEMENT, SMSG_RESPOND_INSPECT_ACHIEVEMENTS | `achievements` panel, `achievement_earned` event |
| Titles | SMSG_TITLE_EARNED | `character.titles`, `title_earned` event |
| Equipment sets | SMSG_EQUIPMENT_SET_LIST / _SAVED / _USE_RESULT | `character.equipment_sets` |
| Glyphs, dual spec | SMSG_TALENTS_INFO, MSG_TALENT_WIPE_CONFIRM, SMSG_TALENTS_INVOLUNTARILY_RESET | `talents.glyphs`, `talents.specs`; talent-wipe prompt |
| Barber shop | SMSG_ENABLE_BARBER_SHOP, SMSG_BARBER_SHOP_RESULT | `interaction.barber` |
| Guild bank | SMSG_GUILD_BANK_LIST, MSG_GUILD_BANK_LOG_QUERY, MSG_QUERY_GUILD_BANK_TEXT, MSG_GUILD_BANK_MONEY_WITHDRAWN | `interaction.guild_bank` |
| Gem sockets | SMSG_SOCKET_GEMS_RESULT | `interaction.socketing` |
| Stable | MSG_LIST_STABLED_PETS, SMSG_STABLE_RESULT | `interaction.stable` |
| Guild charters, tabards | SMSG_PETITION_SHOWLIST, SMSG_PETITION_SHOW_SIGNATURES, SMSG_PETITION_SIGN_RESULTS, SMSG_TURN_IN_PETITION_RESULTS, MSG_TABARDVENDOR_ACTIVATE | `interaction.guild_registrar` / `petition` / `tabard` |
| PvP: honor, arena teams, queues | SMSG_PVP_CREDIT, SMSG_BATTLEFIELD_STATUS, SMSG_BATTLEFIELD_LIST, SMSG_ARENA_TEAM_*, SMSG_BATTLEFIELD_MGR_* (Wintergrasp), MSG_INSPECT_HONOR_STATS | `pvp` panel, `world.queues`, Wintergrasp prompts |
| World states outside battlegrounds | SMSG_INIT_WORLD_STATES, SMSG_UPDATE_WORLD_STATE, SMSG_WORLD_STATE_UI_TIMER_UPDATE | generic `world_states` (dungeon counters, Wintergrasp timer) |
| Who, ignore, channels | SMSG_WHO, SMSG_CONTACT_LIST, SMSG_FRIEND_STATUS, SMSG_CHANNEL_LIST | `social.ignore`, `social.channels`, `who` command + results |
| Temporary weapon enchants | SMSG_ITEM_ENCHANT_TIME_UPDATE, SMSG_ENCHANTMENTLOG | `self.weapon_enchants` |
| Mounts, companions | SMSG_MOUNTRESULT, SMSG_DISMOUNTRESULT | `character.mounts` / `companions`; mount errors as `error` |
| Hearthstone binding | SMSG_BINDER_CONFIRM, SMSG_PLAYERBINDERROR | `bind_hearthstone` prompt |
| Dungeon Finder (on since 2026-11-15) | SMSG_LFG_JOIN_RESULT, SMSG_LFG_UPDATE_PLAYER / _PARTY / _SEARCH, SMSG_LFG_QUEUE_STATUS, SMSG_LFG_PROPOSAL_UPDATE, SMSG_LFG_ROLE_CHECK_UPDATE, SMSG_LFG_ROLE_CHOSEN, SMSG_LFG_BOOT_PROPOSAL_UPDATE, SMSG_LFG_OFFER_CONTINUE, SMSG_LFG_PLAYER_REWARD, SMSG_LFG_PLAYER_INFO, SMSG_LFG_PARTY_INFO | **done in rev 7**: `dungeon_finder` panel, `world.queues`, `LFG_PROPOSAL` / `LFG_BOOT_VOTE` / `LFG_OFFER_CONTINUE` / `ROLE_CHECK` prompts, `lfg` event, `party.lfg`, `group.role` |

## Hand-reviewed (60 the rules missed)

| Opcode(s) | Visible? | Verdict |
|---|---|---|
| MSG_RANDOM_ROLL | yes ("rolls 57 (1-100)") | **gap**: `roll` event |
| SMSG_CROSSED_INEBRIATION_THRESHOLD | yes ("You feel tipsy") | **gap**: `self.drunk` + message |
| SMSG_PLAYERBOUND, SMSG_BINDZONEREPLY | yes | **gap**: `bind_hearthstone` prompt + `hearthstone_bound` event |
| SMSG_KICK_REASON | yes | **gap**: `kicked` event |
| SMSG_AFK_MONITOR_INFO_RESPONSE | yes (reported AFK in a BG) | **gap**: `battleground` event |
| SMSG_NPC_WONT_TALK | yes | `error` event |
| SMSG_NOTIFY_DEST_LOC_SPELL_CAST | yes | Area / `activity.cast` |
| SMSG_RESUME_CAST_BAR, SMSG_AURACASTLOG, SMSG_SPELL_CHANCE_PROC_LOG | yes | cast bar, `combat` summary |
| SMSG_DYNAMIC_DROP_ROLL_RESULT | yes | `trade_loot` roll |
| SMSG_DESTRUCTIBLE_BUILDING_DAMAGE | yes (siege walls) | `battleground` / world states |
| SMSG_BUY_BANK_SLOT_RESULT | yes | `interaction.bank` |
| SMSG_CHANGEPLAYER_DIFFICULTY_RESULT | yes | `instance` |
| SMSG_CAMERA_SHAKE, SMSG_FORCE_ANIM | visual only | not modeled |
| GM, tickets, voice (COMSAT), profiling, SERVER_BUCK, MINIGAME, `*_OBSOLETE`, recruit-a-friend, promo codes, complaint result, channel member lists, test drop rates | no / not applicable | internal or out of scope |
