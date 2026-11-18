"""Drafts docs/specs/client-events.tsv: every 3.3.5a client event, classified.

One-off drafting aid for the client-event audit (docs/specs/coverage.md):
rules by name give each event its kind and where it lands in our snapshot; the
TSV is then reviewed and kept by hand (this script won't overwrite reviewed
rows: it only adds events missing from the TSV, marked `review`).

Kinds (what the event is to a player):
  event       a discrete happening -> an Event in the snapshot's `events`
  prompt      a popup that needs an answer -> PromptEvent
  change      a discrete change of state -> a snapshot field (trigger: it changed)
  value       a continuous value -> a snapshot field (trigger: a watch the brain sets)
  combat_log  a combat log sub-event -> ext.combat (trigger: hits on me, etc.)
  panel       contents of a window the bot opened itself -> its panel
  internal    client plumbing (loading, settings, caches, key bindings)
  out         out of scope (Battle.net, voice chat, GM tools, billing, login screens)
  not_event   not an event (a name the extraction picked up)
  gap         visible to a player, but the snapshot has no place for it yet

Usage: python3 -I tools/audit/classify_client_events.py
"""

import csv
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
EVENTS = ROOT / "out" / "client-ui" / "events_all.txt"
ALERTS = ROOT / "out" / "client-ui" / "alerts.tsv"
TSV = ROOT / "docs" / "specs" / "client-events.tsv"
COLUMNS = ["event", "kind", "maps_to", "client_alert", "note"]

# (regex, kind, maps_to, note). First match wins.
RULES = [
    # Not events.
    (r"^(BLOCK|BOTH|ENERGIZE|EVERYTHING|HEAL|IMMUNE|INCOMING|OUTGOING|OUTGOING_ME|RESET|"
     r"REVERT|SAVE|SYSMSG|WOUND|ZONE_UNDER_ATTACK|NEW_TITLE_EARNED|OLD_TITLE_LOST|"
     r"SHOW_COMPARE_TOOLTIP|FRIENDLIST_SHOW|ARENA_TEAM_INVITE_CANCEL|"
     r"KNOWLEDGE_BASE_SYSTEM_MOTD_UPDATE)$", "not_event", "",
     "a combat-text type, chat type or UI-internal name, not a client event"),
    # Out of scope.
    (r"^(BN_|CHAT_MSG_BN_)", "out", "", "Battle.net (not in 3.3.5a servers)"),
    (r"^VOICE_|^CHANNEL_VOICE_UPDATE$", "out", "", "voice chat"),
    (r"^(GM|GMSURVEY|GMRESPONSE|UPDATE_GM_STATUS|UPDATE_TICKET|KNOWLEDGE_BASE_|"
     r"SHOW_SURVEY_NOTIFICATION|GM_PLAYER_INFO)", "out", "", "GM tools and tickets"),
    (r"^(BILLING_NAG_DIALOG|IGR_BILLING_NAG_DIALOG|ACCOUNT_MESSAGES_|CLIENT_TRIAL|"
     r"CLIENT_ACCOUNT_MISMATCH|PLAYTIME_CHANGED|SCANDLL_|PATCH_|GAME_ACCOUNTS_UPDATED|"
     r"OPEN_REALM_LIST|SUGGEST_REALM|GET_PREFERRED_REALM_INFO|CHARACTER_LIST_UPDATE|"
     r"SELECT_(FIRST|LAST)_CHARACTER|UPDATE_SELECTED_(CHARACTER|RACE)|SET_GLUE_SCREEN|"
     r"START_GLUE_MUSIC|GLUE_|FORCE_(DECLINE|RENAME)_CHARACTER|OPEN_STATUS_DIALOG|"
     r"UPDATE_STATUS_DIALOG|CLOSE_STATUS_DIALOG|SERVER_SPLIT_NOTICE|PLAYER_ENTER_(PIN|MATRIX|TOKEN))",
     "out", "", "login screens, billing, account"),
    (r"^COMMENTATOR_", "out", "", "arena commentator mode"),
    (r"^LEVEL_GRANT_PROPOSED$", "out", "", "recruit-a-friend level grant"),
    (r"^(MOVIE_|ENABLE_TAXI_BENCHMARK|DISABLE_TAXI_BENCHMARK|SCREENSHOT_)", "out", "",
     "client recording and benchmarks"),
    # Internal.
    (r"^(ADDON_\w+|VARIABLES_LOADED|FRAMES_LOADED|CVAR_UPDATE|DISPLAY_SIZE_CHANGED|"
     r"SOUND_DEVICE_UPDATE|UPDATE_BINDINGS|MODIFIER_STATE_CHANGED|CURSOR_UPDATE|"
     r"SYNCHRONIZE_SETTINGS|UPDATE_CHAT_COLOR|UPDATE_CHAT_COLOR_NAME_BY_CLASS|"
     r"UPDATE_CHAT_WINDOWS|UPDATE_FLOATING_CHAT_WINDOWS|EXECUTE_CHAT_LINE|"
     r"MACRO_ACTION_\w+|UPDATE_MACROS|TIMER_ALERT|WOW_MOUSE_NOT_FOUND|TUTORIAL_TRIGGER|"
     r"PLAYER_LOGIN|PLAYER_LOGOUT|PLAYER_LEAVING_WORLD|DISCONNECTED_FROM_SERVER|"
     r"QUEST_QUERY_COMPLETE|ITEM_LOCK_CHANGED|ITEM_LOCKED|ITEM_UNLOCKED|"
     r"ACTIONBAR_SHOWGRID|ACTIONBAR_HIDEGRID|PET_BAR_SHOWGRID|PET_BAR_HIDEGRID|"
     r"UPDATE_MOUSEOVER_UNIT|COMBAT_TEXT_UPDATE|COMBAT_LOG_EVENT$|"
     r"PLAYER_DAMAGE_DONE_MODS|UNIT_PORTRAIT_UPDATE|UNIT_MODEL_CHANGED|UNIT_NAME_UPDATE|"
     r"LANGUAGE_LIST_CHANGED|PET_FORCE_NAME_DECLENSION|MINIMAP_UPDATE_ZOOM|"
     r"CHAT_MSG_ADDON|CHAT_MSG_FILTERED|CHAT_MSG_RESTRICTED|TIME_PLAYED_MSG)$",
     "internal", "", "client plumbing"),
    # Combat log sub-events.
    (r"^(SWING|RANGE|SPELL|SPELL_PERIODIC|SPELL_BUILDING|ENVIRONMENTAL|DAMAGE)_"
     r"(DAMAGE|MISSED|HEAL|ENERGIZE|DRAIN|LEECH|AURA_\w+|CAST_\w+|DISPEL\w*|INTERRUPT|"
     r"EXTRA_ATTACKS|INSTAKILL|DURABILITY_DAMAGE\w*|CREATE|SUMMON|RESURRECT|STOLEN|"
     r"SHIELD\w*|SPLIT|AURA_BROKEN\w*)$|^(UNIT_DIED|UNIT_DESTROYED|UNIT_DISSIPATES|PARTY_KILL)$",
     "combat_log", "ext.combat", "combat log sub-event"),
    (r"^COMBAT_LOG_EVENT_UNFILTERED$", "combat_log", "ext.combat",
     "the combat log: every hit, heal, aura and cast around the player"),
    # Chat.
    (r"^CHAT_MSG_(SAY|YELL|EMOTE|TEXT_EMOTE|WHISPER|WHISPER_INFORM|PARTY|PARTY_LEADER|RAID|"
     r"RAID_LEADER|RAID_WARNING|GUILD|OFFICER|CHANNEL|BATTLEGROUND|BATTLEGROUND_LEADER|"
     r"MONSTER_\w+|RAID_BOSS_\w+|ACHIEVEMENT|GUILD_ACHIEVEMENT|AFK|DND|IGNORED|"
     r"CHANNEL_\w+|TARGETICONS)$", "event", "event.chat", ""),
    (r"^CHAT_MSG_(SYSTEM|BG_SYSTEM_\w+|OPENING|PET_INFO|COMBAT_MISC_INFO)$", "event",
     "event.info", "system text"),
    (r"^CHAT_MSG_LOOT$", "event", "event.item_received", "loot lines (mine and others')"),
    (r"^CHAT_MSG_MONEY$", "event", "event.money_changed", ""),
    (r"^CHAT_MSG_COMBAT_XP_GAIN$", "event", "event.xp_gained", ""),
    (r"^CHAT_MSG_SKILL$", "event", "event.skill_up", ""),
    (r"^CHAT_MSG_COMBAT_HONOR_GAIN$", "event", "event.battleground", "honor gained"),
    (r"^CHAT_MSG_COMBAT_FACTION_CHANGE$", "event", "event.info", "reputation change line"),
    (r"^CHAT_MSG_TRADESKILLS$", "event", "event.info", "crafting lines"),
    (r"^(UI_ERROR_MESSAGE)$", "event", "event.error", "red error text"),
    (r"^(UI_INFO_MESSAGE)$", "event", "event.info", "yellow info text"),
    # Popups (prompts).
    (r"^PARTY_INVITE_REQUEST$", "prompt", "prompt.GROUP_INVITE", ""),
    (r"^TRADE_REQUEST$|^TRADE_SHOW$", "prompt", "prompt.TRADE", ""),
    (r"^DUEL_REQUESTED$", "prompt", "prompt.DUEL", ""),
    (r"^GUILD_INVITE_REQUEST$", "prompt", "prompt.GUILD_INVITE", ""),
    (r"^READY_CHECK$", "prompt", "prompt.READY_CHECK", ""),
    (r"^LFG_ROLE_CHECK_SHOW$", "prompt", "prompt.ROLE_CHECK", ""),
    (r"^RESURRECT_REQUEST$", "prompt", "prompt.RESURRECT_OFFER", ""),
    (r"^CONFIRM_SUMMON$", "prompt", "prompt.SUMMON", ""),
    (r"^QUEST_ACCEPT_CONFIRM$", "prompt", "prompt.QUEST_SHARE", ""),
    (r"^ARENA_TEAM_INVITE_REQUEST$", "prompt", "prompt.ARENA_TEAM_INVITE", ""),
    (r"^BATTLEFIELD_MGR_(ENTRY_INVITE|QUEUE_INVITE)$", "prompt", "prompt.WINTERGRASP_INVITE", ""),
    (r"^CONFIRM_BINDER$", "prompt", "prompt.BIND_HEARTHSTONE", ""),
    (r"^(LOOT_BIND_CONFIRM|EQUIP_BIND_CONFIRM|AUTOEQUIP_BIND_CONFIRM|USE_BIND_CONFIRM)$",
     "prompt", "prompt.CONFIRM_BIND_ON_PICKUP", ""),
    (r"^DELETE_ITEM_CONFIRM$", "prompt", "prompt.CONFIRM_DELETE_ITEM", ""),
    (r"^(REPLACE_ENCHANT|TRADE_REPLACE_ENCHANT|BIND_ENCHANT|TRADE_POTENTIAL_BIND_ENCHANT)$",
     "prompt", "prompt.CONFIRM_REPLACE_ENCHANT", ""),
    (r"^CONFIRM_TALENT_WIPE$", "prompt", "prompt.CONFIRM_TALENT_WIPE", ""),
    (r"^(CONFIRM_LOOT_ROLL|CONFIRM_DISENCHANT_ROLL)$", "prompt", "prompt.CONFIRM_LOOT_ROLL", ""),
    (r"^INSTANCE_LOCK_START$", "prompt", "prompt.CONFIRM_INSTANCE_LOCK", ""),
    (r"^CONFIRM_XP_LOSS$", "prompt", "prompt.CONFIRM_SPIRIT_HEALER", ""),
    (r"^LFG_PROPOSAL_SHOW$", "prompt", "prompt.LFG_PROPOSAL", ""),
    (r"^LFG_BOOT_PROPOSAL_UPDATE$", "prompt", "prompt.LFG_BOOT_VOTE", ""),
    (r"^LFG_OFFER_CONTINUE$", "prompt", "prompt.LFG_OFFER_CONTINUE", ""),
    (r"^(END_BOUND_TRADEABLE|END_REFUND)$", "prompt", "prompt.CONFIRM_PURCHASE",
     "confirm making a refundable/tradeable item permanent"),
    # Found by the alert cross-check (gaps, since filled).
    (r"^AREA_SPIRIT_HEALER_IN_RANGE$", "prompt", "prompt.SPIRIT_HEALER_WAVE", ""),
    (r"^CORPSE_IN_RANGE$", "prompt", "prompt.RECLAIM_CORPSE", ""),
    (r"^CHANNEL_INVITE_REQUEST$", "prompt", "prompt.CHANNEL_INVITE", ""),
    (r"^CHANNEL_PASSWORD_REQUEST$", "prompt", "prompt.CHANNEL_PASSWORD", ""),
    (r"^PET_RENAMEABLE$", "prompt", "prompt.PET_NAME", ""),
    (r"^MINIMAP_PING$", "event", "event.minimap_ping", ""),
    (r"^CALENDAR_EVENT_ALARM$", "event", "event.calendar_alarm", ""),
    (r"^(CORPSE_IN_INSTANCE)$", "event", "event.info", "'your corpse is in the instance'"),
    (r"^(CALENDAR_UPDATE_ERROR|AUCTION_HOUSE_DISABLED|WEAR_EQUIPMENT_SET)$", "event",
     "event.error", "red error or error popup"),
    # Discrete happenings.
    (r"^PLAYER_LEVEL_UP$", "event", "event.level_up", ""),
    (r"^PLAYER_XP_UPDATE$", "change", "base.self.xp", ""),
    (r"^ACHIEVEMENT_EARNED$", "event", "event.achievement_earned", ""),
    (r"^QUEST_ACCEPTED$", "event", "event.quest_accepted", ""),
    (r"^PLAYER_DEAD$", "event", "event.died", ""),
    (r"^(PLAYER_ALIVE|PLAYER_UNGHOST)$", "event", "event.resurrected", ""),
    (r"^START_LOOT_ROLL$", "event", "event.loot_roll", ""),
    (r"^CANCEL_LOOT_ROLL$", "change", "ext.trade_loot", "a roll ended"),
    (r"^ITEM_PUSH$", "event", "event.item_received", ""),
    (r"^(ZONE_CHANGED|ZONE_CHANGED_INDOORS|ZONE_CHANGED_NEW_AREA)$", "event",
     "event.zone_changed", ""),
    (r"^(RAID_INSTANCE_WELCOME|UPDATE_INSTANCE_INFO|INSTANCE_BOOT_START|INSTANCE_BOOT_STOP|"
     r"INSTANCE_LOCK_STOP|PLAYER_DIFFICULTY_CHANGED)$", "event", "event.instance", ""),
    (r"^(DUEL_FINISHED|DUEL_INBOUNDS|DUEL_OUTOFBOUNDS)$", "event", "event.duel", ""),
    (r"^CINEMATIC_(START|STOP)$|^PLAY_MOVIE$", "event", "event.cinematic", ""),
    (r"^CHARACTER_POINTS_CHANGED$", "event", "event.talent_point_gained", ""),
    (r"^(LFG_PROPOSAL_(FAILED|SUCCEEDED|UPDATE)|LFG_COMPLETION_REWARD|LFG_UPDATE|"
     r"LFG_QUEUE_STATUS_UPDATE|LFG_ROLE_CHECK_(HIDE|ROLE_CHOSEN|UPDATE)|PARTY_LFG_RESTRICTED)$",
     "event", "event.lfg", ""),
    (r"^(UPDATE_BATTLEFIELD_STATUS|PLAYER_ENTERING_BATTLEGROUND|BATTLEFIELD_MGR_\w+)$", "event",
     "event.battleground", ""),
    (r"^(NEW_AUCTION_UPDATE|AUCTION_MULTISELL_\w+)$", "event", "event.auction", ""),
    (r"^(UPDATE_PENDING_MAIL)$", "event", "event.mail_received", ""),
    (r"^(MAIL_SEND_SUCCESS|MAIL_FAILED|MAIL_SUCCESS)$", "event", "event.info", "mail result"),
    (r"^TALENTS_INVOLUNTARILY_RESET$", "event", "event.info", ""),
    (r"^(PLAYER_SKINNED)$", "event", "event.info", "your corpse was looted for insignia (PvP)"),
    (r"^(SHOW_SERVER_ALERT)$", "event", "event.info", "server announcement"),
    (r"^PLAYER_ENTERING_WORLD$", "event", "event.zone_changed",
     "after a loading screen: login, teleport, instance portal"),
    (r"^(PLAYER_CAMPING|PLAYER_QUITING|LOGOUT_CANCEL)$", "change", "base.self.camping", ""),
    # Discrete state changes of the bot itself.
    (r"^(PLAYER_REGEN_DISABLED|PLAYER_REGEN_ENABLED|PLAYER_ENTER_COMBAT|PLAYER_LEAVE_COMBAT)$",
     "change", "base.self.in_combat", "entering / leaving combat; auto-attack on/off"),
    (r"^(PLAYER_CONTROL_LOST|PLAYER_CONTROL_GAINED)$", "change", "base.self.control",
     "feared, charmed, stunned, on a taxi"),
    (r"^(PLAYER_TARGET_CHANGED)$", "change", "ext.target", ""),
    (r"^(PLAYER_FARSIGHT_FOCUS_CHANGED)$", "change", "base.self.control",
     "the camera moved to another unit (Eye of Kilrogg, Far Sight)"),
    (r"^(PLAYER_FOCUS_CHANGED)$", "change", "ext.target", "focus"),
    (r"^(UNIT_TARGET)$", "change", "base.nearby.units", "who a unit is targeting"),
    (r"^(UNIT_THREAT_LIST_UPDATE|UNIT_THREAT_SITUATION_UPDATE)$", "change", "base.nearby.units",
     "threat (aggro) on units"),
    (r"^(UNIT_AURA|PLAYER_AURAS_CHANGED|UPDATE_STEALTH)$", "change", "base.self.unit",
     "auras (buffs, debuffs, crowd control)"),
    (r"^(UNIT_SPELLCAST_\w+|CURRENT_SPELL_CAST_CHANGED|START_AUTOREPEAT_SPELL|"
     r"STOP_AUTOREPEAT_SPELL)$", "change", "base.self.casting", "casts (mine and units')"),
    (r"^(UNIT_COMBAT)$", "change", "ext.combat",
     "damage/heal shown on a portrait (combat feedback)"),
    (r"^(UNIT_ATTACK|AUTOFOLLOW_BEGIN|AUTOFOLLOW_END)$", "change", "base.self.auto_attack", ""),
    (r"^(UPDATE_SHAPESHIFT_\w+)$", "change", "base.self.form", ""),
    (r"^(PLAYER_UPDATE_RESTING|UPDATE_EXHAUSTION)$", "change", "base.self.resting", ""),
    (r"^(PLAYER_FLAGS_CHANGED)$", "change", "base.self.pvp_flagged", "PvP flag, AFK/DND"),
    (r"^(UNIT_FACTION|UNIT_FLAGS|UNIT_CLASSIFICATION_CHANGED|UNIT_LEVEL|UNIT_DISPLAYPOWER)$",
     "change", "base.nearby.units", "a unit's reaction, flags, level"),
    (r"^(UNIT_ENTERED_VEHICLE|UNIT_ENTERING_VEHICLE|UNIT_EXITED_VEHICLE|UNIT_EXITING_VEHICLE|"
     r"VEHICLE_\w+|PLAYER_GAINS_VEHICLE_DATA|PLAYER_LOSES_VEHICLE_DATA|"
     r"UPDATE_BONUS_ACTIONBAR)$", "change", "ext.vehicle", ""),
    (r"^(MIRROR_TIMER_\w+)$", "change", "ext.environment", "breath, fatigue, feign death bars"),
    (r"^(PLAYER_EQUIPMENT_CHANGED|UNIT_INVENTORY_CHANGED|UPDATE_INVENTORY_DURABILITY|"
     r"UPDATE_INVENTORY_ALERTS|EQUIPMENT_SETS_CHANGED|EQUIPMENT_SWAP_\w+|WEAR_EQUIPMENT_SET|"
     r"ENCHANT_APPLIED|ENCHANT_REMOVED)$", "change", "ext.character", "gear, durability"),
    (r"^(BAG_UPDATE|BAG_UPDATE_COOLDOWN|BAG_OPEN|BAG_CLOSED)$", "change", "ext.bags", ""),
    (r"^(PLAYER_MONEY|PLAYER_TRADE_MONEY)$", "change", "base.self.money", ""),
    (r"^(SPELLS_CHANGED|LEARNED_SPELL_IN_TAB)$", "change", "ext.spellbook", ""),
    (r"^(SKILL_LINES_CHANGED)$", "change", "ext.skills", ""),
    (r"^(UPDATE_FACTION)$", "change", "ext.reputation", ""),
    (r"^(ACTIVE_TALENT_GROUP_CHANGED|PLAYER_TALENT_UPDATE|PREVIEW_TALENT_POINTS_CHANGED|"
     r"GLYPH_\w+|USE_GLYPH)$", "change", "ext.talents", ""),
    (r"^(QUEST_LOG_UPDATE|QUEST_WATCH_UPDATE|QUEST_POI_UPDATE|UNIT_QUEST_LOG_CHANGED|"
     r"QUEST_ITEM_UPDATE)$", "change", "ext.quest_log", "quest progress"),
    (r"^(PARTY_MEMBERS_CHANGED|PARTY_LEADER_CHANGED|PARTY_LOOT_METHOD_CHANGED|"
     r"PARTY_MEMBER_(ENABLE|DISABLE)|PARTY_CONVERTED_TO_RAID|PLAYER_ROLES_ASSIGNED|"
     r"LFG_ROLE_UPDATE)$", "change", "ext.party", ""),
    (r"^(RAID_ROSTER_UPDATE|RAID_TARGET_UPDATE|READY_CHECK_CONFIRM|READY_CHECK_FINISHED)$",
     "change", "ext.raid", "roster, raid markers, ready check answers"),
    (r"^(INSTANCE_ENCOUNTER_ENGAGE_UNIT)$", "change", "ext.encounter", "boss frames"),
    (r"^(UPDATE_WORLD_STATES|WORLD_STATE_UI_TIMER_UPDATE|UPDATE_BATTLEFIELD_SCORE)$", "change",
     "ext.battleground", "world states, scores"),
    (r"^(ARENA_OPPONENT_UPDATE|ARENA_SEASON_WORLD_STATE)$", "change", "ext.arena", ""),
    (r"^(ARENA_TEAM_ROSTER_UPDATE|ARENA_TEAM_UPDATE|HONOR_CURRENCY_UPDATE|"
     r"PLAYER_PVP_KILLS_CHANGED|PLAYER_PVP_RANK_CHANGED|INSPECT_HONOR_UPDATE|"
     r"PVPQUEUE_ANYWHERE_\w+|NPC_PVPQUEUE_ANYWHERE)$", "change", "ext.pvp", ""),
    (r"^(PET_\w+|UNIT_PET|UNIT_HAPPINESS|UNIT_MAXHAPPINESS|UNIT_PET_EXPERIENCE|"
     r"LOCALPLAYER_PET_RENAMED|PREVIEW_PET_TALENT_POINTS_CHANGED)$", "change", "ext.pet", ""),
    (r"^(PLAYER_TOTEM_UPDATE)$", "change", "ext.pet", "totems"),
    (r"^(RUNE_POWER_UPDATE|RUNE_TYPE_UPDATE)$", "value", "base.self.runes", ""),
    (r"^(COMPANION_\w+)$", "change", "ext.companions", ""),
    (r"^(KNOWN_TITLES_UPDATE|KNOWN_CURRENCY_TYPES_UPDATE|CURRENCY_DISPLAY_UPDATE)$", "change",
     "ext.character", "titles, currencies"),
    (r"^(CRITERIA_UPDATE|TRACKED_ACHIEVEMENT_UPDATE|RECEIVED_ACHIEVEMENT_LIST|"
     r"INSPECT_ACHIEVEMENT_READY)$", "change", "ext.achievements", ""),
    (r"^(FRIENDLIST_UPDATE|IGNORELIST_UPDATE|MUTELIST_UPDATE|GUILD_ROSTER_UPDATE|GUILD_MOTD|"
     r"GUILD_EVENT_LOG_UPDATE|PLAYER_GUILD_UPDATE|WHO_LIST_UPDATE|CHANNEL_\w+|"
     r"GUILD_INVITE_CANCEL|PARTY_INVITE_CANCEL|TRADE_REQUEST_CANCEL|CANCEL_SUMMON)$", "change",
     "ext.social", "friends online, guild, channels"),
    (r"^(CALENDAR_\w+)$", "change", "ext.calendar", ""),
    (r"^(LFG_LOCK_INFO_RECEIVED|LFG_UPDATE_RANDOM_INFO|LFG_OPEN_FROM_GOSSIP|UPDATE_LFG_\w+)$",
     "change", "ext.dungeon_finder", ""),
    (r"^(MINIMAP_PING|MINIMAP_UPDATE_TRACKING)$", "change", "ext.minimap", ""),
    (r"^(WORLD_MAP_UPDATE|WORLD_MAP_NAME_UPDATE|CLOSE_WORLD_MAP)$", "panel", "ext.map", ""),
    (r"^(CORPSE_IN_RANGE|CORPSE_OUT_OF_RANGE|CORPSE_IN_INSTANCE|AREA_SPIRIT_HEALER_\w+|"
     r"RAISED_AS_GHOUL)$", "change", "ext.death", ""),
    (r"^(DISABLE_XP_GAIN|ENABLE_XP_GAIN|DISABLE_LOW_LEVEL_RAID|ENABLE_LOW_LEVEL_RAID)$",
     "change", "ext.character", ""),
    (r"^(UNIT_STATS|UNIT_DAMAGE|UNIT_ATTACK_POWER|UNIT_ATTACK_SPEED|UNIT_RANGED\w+|"
     r"UNIT_RESISTANCES|UNIT_DEFENSE|COMBAT_RATING_UPDATE|PET_SPELL_POWER_UPDATE)$", "change",
     "ext.character", "stats on the character sheet"),
    # Continuous values.
    (r"^(UNIT_HEALTH|UNIT_MAXHEALTH|UNIT_MANA|UNIT_MAXMANA|UNIT_RAGE|UNIT_MAXRAGE|UNIT_ENERGY|"
     r"UNIT_MAXENERGY|UNIT_FOCUS|UNIT_MAXFOCUS|UNIT_RUNIC_POWER|UNIT_MAXRUNIC_POWER|"
     r"UNIT_COMBO_POINTS)$", "value", "base.self", "health and power (mine and units')"),
    (r"^(ACTIONBAR_\w+|SPELL_UPDATE_COOLDOWN|SPELL_UPDATE_USABLE|UPDATE_MULTI_CAST_ACTIONBAR|"
     r"UPDATE_TRADESKILL_RECAST)$", "value", "ext.action_bars", "cooldowns, usable, range"),
    # Windows the bot opened itself.
    (r"^(MERCHANT_\w+)$", "panel", "ext.interaction", "vendor"),
    (r"^(GOSSIP_SHOW|GOSSIP_CLOSED|QUEST_GREETING|QUEST_DETAIL|QUEST_PROGRESS|QUEST_COMPLETE|"
     r"QUEST_FINISHED|GOSSIP_CONFIRM|GOSSIP_CONFIRM_CANCEL|GOSSIP_ENTER_CODE)$", "panel",
     "ext.interaction", "NPC dialog"),
    (r"^(TRAINER_\w+)$", "panel", "ext.interaction", "trainer"),
    (r"^(TAXIMAP_\w+)$", "panel", "ext.interaction", "flight master"),
    (r"^(BANKFRAME_\w+|PLAYERBANKSLOTS_CHANGED|PLAYERBANKBAGSLOTS_CHANGED)$", "panel",
     "ext.interaction", "bank"),
    (r"^(GUILDBANK\w+)$", "panel", "ext.interaction", "guild bank"),
    (r"^(AUCTION_\w+)$", "panel", "ext.interaction", "auction house"),
    (r"^(MAIL_\w+|CLOSE_INBOX_ITEM|SEND_MAIL_\w+)$", "panel", "ext.interaction", "mailbox"),
    (r"^(TRADE_SKILL_\w+)$", "panel", "ext.interaction", "profession window"),
    (r"^(TRADE_\w+)$", "panel", "ext.trade_loot", "trade window"),
    (r"^(LOOT_OPENED|LOOT_CLOSED|LOOT_SLOT_\w+|OPEN_MASTER_LOOT_LIST|UPDATE_MASTER_LOOT_LIST)$",
     "panel", "ext.trade_loot", "loot window"),
    (r"^(BARBER_SHOP_\w+)$", "panel", "ext.interaction", "barber"),
    (r"^(PETITION_\w+|GUILD_REGISTRAR_\w+|OPEN_TABARD_FRAME|CLOSE_TABARD_FRAME|"
     r"TABARD_\w+|GUILDTABARD_UPDATE)$", "panel", "ext.interaction", "charters, tabards"),
    (r"^(SOCKET_INFO_\w+)$", "panel", "ext.interaction", "gem sockets"),
    (r"^(ITEM_TEXT_\w+)$", "panel", "ext.interaction", "books and letters"),
    (r"^(BATTLEFIELDS_SHOW|BATTLEFIELDS_CLOSED)$", "panel", "ext.interaction", "battlemaster"),
    (r"^(INSPECT_TALENT_READY)$", "panel", "ext.interaction", "inspect"),
    (r"^(VOTE_KICK_REASON_NEEDED)$", "panel", "ext.dungeon_finder",
     "asks for a reason after the bot starts a vote kick"),
    (r"^(START_MINIGAME|MINIGAME_UPDATE)$", "out", "", "unused in 3.3.5a"),
]


def classify(event):
    for pattern, kind, maps_to, note in RULES:
        if re.search(pattern, event):
            return kind, maps_to, note
    return "review", "", ""


def main():
    alerts = {}
    for line in ALERTS.read_text().splitlines():
        event, kinds, *_ = line.split("\t")
        if kinds:
            alerts[event] = kinds
    rows = {}
    if TSV.exists():
        with open(TSV) as f:
            rows = {r["event"]: r for r in csv.DictReader(f, delimiter="\t")}
    added = 0
    for event in EVENTS.read_text().split():
        if event in rows:
            rows[event]["client_alert"] = alerts.get(event, "")  # Facts refresh.
            continue
        kind, maps_to, note = classify(event)
        rows[event] = {"event": event, "kind": kind, "maps_to": maps_to,
                       "client_alert": alerts.get(event, ""), "note": note}
        added += 1
    with open(TSV, "w", newline="") as f:
        w = csv.DictWriter(f, COLUMNS, delimiter="\t", lineterminator="\n")
        w.writeheader()
        for event in sorted(rows):
            w.writerow(rows[event])
    review = [e for e, r in rows.items() if r["kind"] == "review"]
    print(f"{len(rows)} events ({added} new), {len(review)} to review: {' '.join(review)}")


if __name__ == "__main__":
    main()
