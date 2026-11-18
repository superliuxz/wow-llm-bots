# Snapshot coverage against the client

Three audits that check `snapshot.md` misses nothing a player can see or be told: every
server-to-client opcode, every client event (the full list of triggers) and every UI frame of
the 3.3.5a client. Each section was its own file until 2026-11-18.

## Server-to-client opcodes
Audit of `snapshot.md` (rev 6) against every `SMSG_*` and `MSG_*` opcode in AzerothCore's
`Opcodes.h` (688). A packet the server sends to a player is something the client can show;
each one is either a snapshot field or event, internal to the protocol, or out of scope.

Method: rules by prefix and keyword classified 628 opcodes; the 60 left over were reviewed
by hand; then every "mapped" opcode for a feature was re-checked against an actual field in
the spec. Working files (untracked): `out/audit/opcodes_classified.tsv`.

### Summary

| Class | Count | Meaning |
|---|---|---|
| internal | 194 | protocol, caches, client config, or folded into the 1 s state read |
| state | 66 | read from server state each second, not from the packet |
| mapped, field exists | ~300 | a spec field or event carries it |
| mapped, **no field yet** | ~70 | the feature is visible but the spec lacks it (below) |
| hand-reviewed | 60 | below |

### Mapped, but no field yet

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

### Hand-reviewed (60 the rules missed)

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

## Client events (the full list of triggers)
The brain is woken by triggers: something happened that the bot's current plan didn't account
for. For that to work, the list of things that can happen must be **complete**. This audit
builds it from the 3.3.5a client itself: every event the client can fire is something a player
can be told about, and nothing else is.

### Sources

`tools/wlb setup client-ui` (needs the client and a server build) extracts, into the
untracked `out/client-ui/`:

- **The default UI code** (FrameXML, GlueXML, the Blizzard_* addons: 463 files) from the
  client's MPQs, with AzerothCore's libmpq.
- **Every event name**: the event tables in `Wow.exe` (688 names), plus every event the UI
  registers or compares against (659, including the chat events it builds from
  `ChatTypeGroup`): **713 together**.

### The table

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

### Client alerts

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

### Gaps found (all filled)

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

### Enforced

`tools/wlb check snapshots` runs `tools/audit/check_client_events.py`, which fails when:
a row isn't classified; a `maps_to` names something the proto doesn't have; an Event or
PromptEvent kind in the proto is produced by no client event and isn't listed as coming from
elsewhere (with the reason: our own path failures, DBM warnings, lines split out of chat text);
or, when the client data is extracted, the table and the client's event list differ.

### Played scenarios (to run once triggers are built; `triggers.md`)

Fixture tests on the live server, each checking that the bot's brain is woken, with what, and
how fast: a test character attacks Testpriest mid-quest (gank); a mob stuns it; a whisper; a
group invite; a ready check; a knockback; health drops under a watched line; an Alliance player
comes into view with a watch set; the bot dies; a Dungeon Finder proposal.

### Re-running

```
tools/wlb setup client-ui                       # client data -> out/client-ui
python3 -I tools/audit/client_alerts.py          # alerts per event
python3 -I tools/audit/classify_client_events.py # adds new events to the table, as `review`
tools/wlb check snapshots                      # checks the table
```

## The client's UI frames
Audit of `snapshot.md` (rev 6) against every UI frame the 3.3.5a client ships
(`Interface/FrameXML` and `Interface/AddOns/Blizzard_*` from the MPQs: 179 XML files, 149
after dropping templates, fonts and helpers). Each frame shows something a player can see;
the snapshot must have a place for it, or the frame must be marked out of scope with a
reason.

Status: **covered** (spec location exists), **thin** (exists, missing fields), **gap**
(no place yet), **out** (out of scope), **widget** (no game information of its own).

### Covered

| Frame(s) | Spec location |
|---|---|
| PlayerFrame, UnitFrame, TextStatusBar | `self` (health, power, level, leader, combat, rest, PvP flag) |
| BuffFrame | `self.auras` |
| CastingBarFrame | `self.activity.cast` |
| TargetFrame, FocusFrame, ThreatMeter | `target` (+ focus, ToT, threat) |
| PartyFrame | `party` |
| RaidFrame, Blizzard_RaidUI | `raid`, `social` raid tab |
| PetFrame, PetActionBarFrame, TotemFrame | `pet` |
| ComboFrame | `combat.resources` |
| ActionBarFrame, MultiActionBars, MainMenuBar | `action_bars`, `self.xp` |
| MainMenuBarMicroButtons, MainMenuBarBagButtons, DurabilityFrame | `self.indicators` |
| Minimap, GameTime, QuestPOI | `minimap` (+ `map` objective areas) |
| WatchFrame, QuestTimerFrame | `quest_tracker` |
| WorldFrame | `nearby`, `surroundings` |
| ZoneText | `where`, `zone_changed` event |
| GameTooltip | `tooltips` |
| ChatFrame, RaidWarning | `chat` event |
| UIErrorsFrame | `error` event |
| ReadyCheck | `prompt` event |
| MirrorTimer | `environment` |
| Blizzard_VehicleUI, VehicleMenuBar | `vehicle` |
| CinematicFrame, MovieFrame | `self.in_cinematic`, cinematic events |
| ContainerFrame | `bags` |
| CharacterFrame, PaperDollFrame, StatsFrame | `character` |
| QuestLogFrame | `quest_log` |
| SpellBookFrame | `spellbook` |
| Blizzard_TalentUI | `talents` |
| SkillFrame | `skills` |
| ReputationFrame | `reputation` |
| Blizzard_Calendar | `calendar` |
| WorldMapFrame, Blizzard_BattlefieldMinimap | `map` |
| LootFrame, TradeFrame | `trade_loot` |
| MerchantFrame, GossipFrame, QuestFrame, QuestInfo, TaxiFrame, BankFrame, MailFrame, ItemTextFrame, Blizzard_TrainerUI, Blizzard_TradeSkillUI, Blizzard_AuctionUI | `interaction` |
| LFDFrame | `dungeon_finder` panel; queue in `world.queues`; proposal / boot vote / continue prompts |
| LFGFrame, LFRFrame | `social` LFG listings, `interaction` raid browser |
| CombatLog, Blizzard_CombatLog, Blizzard_CombatText, CombatFeedback | `combat` recent summary |
| PVPBattlegroundFrame (in a battleground) | `battleground` |

### Thin (exists, missing fields)

| Frame | Missing |
|---|---|
| BonusActionBarFrame | stance / shapeshift / stealth bar in `action_bars` |
| MultiCastActionBarFrame | shaman totem-call bar in `action_bars` |
| RuneFrame | DK runes in `self` (visible out of combat too), not only `combat` |
| AlternatePowerBar | druid mana while shapeshifted, in `self` |
| BuffFrame | temporary weapon enchants (poisons, oils, shaman imbues) with time left |
| MerchantFrame | buyback tab |
| Blizzard_InspectUI, InspectPaperDollFrame, InspectTalentFrame, InspectHonorFrame, InspectPVPFrame | concrete fields: gear, talents, honor/arena, achievement comparison |
| PetPaperDollFrame | pet stats and pet talents |
| FriendsFrame | ignore list, `/who` search and results, joined chat channels |
| ChannelFrame | joined channels and their member counts |
| StaticPopup | complete `prompt` kinds: bind hearthstone, bind-on-pickup loot, delete item, abandon quest, replace enchant, release spirit, confirm talent wipe, etc. |
| Blizzard_TalentUI | dual spec (active / inactive), pet talents |
| ItemRef | chat links: items / spells / quests / achievements in a `chat` message as typed objects |
| WorldStateFrame | generic world states anywhere, not only battlegrounds: Wintergrasp timer, Halaa, dungeon counters (Violet Hold portals, Culling of Stratholme timer) |

### Gap (no place yet)

| Frame | Proposed place |
|---|---|
| Blizzard_AchievementUI, AlertFrames | `achievements` panel (categories, points, progress); tracked achievements in `quest_tracker`; `achievement_earned` event and toast |
| CharacterFrame titles | `character.titles`, `title_earned` event |
| Blizzard_TokenUI | `character.currency` (emblems, honor, arena points, badges, tokens) |
| Blizzard_GlyphUI | `talents.glyphs` |
| CharacterFrame equipment manager | `character.equipment_sets` |
| CharacterFrame pets tab | `character.companions` and `character.mounts` |
| Blizzard_BarberShopUI | `interaction.barber` |
| Blizzard_GuildBankUI | `interaction.guild_bank` (tabs, items, money, log) |
| Blizzard_ItemSocketingUI | `interaction.socketing` |
| PetStable | `interaction.stable` |
| GuildRegistrarFrame, PetitionFrame, TabardFrame | `interaction.guild_registrar`, `interaction.petition`, `interaction.tabard` |
| PVPFrame, HonorFrame, BattlefieldFrame | `pvp` panel (honor, kills, arena teams); battleground queue status (minimap eye) in `world.queues` |
| ArenaFrame, ArenaRegistrarFrame, Blizzard_ArenaUI | same `pvp` panel and `interaction` kinds; arena enemy frames deferred with arenas |

### Out of scope

| Frame(s) | Reason |
|---|---|
| AudioOptions*, VideoOptions*, SoundOptionsFrame, VoiceOptionsFrame, InterfaceOptions*, UIOptions*, OptionsFrame, MacOptionsFrame, ChatConfigFrame, Blizzard_BindingUI, ColorPickerFrame, OpacitySliderFrame | client settings; no game information |
| GameMenuFrame | logout / exit; the session is controlled by the harness |
| HelpFrame, KnowledgeBaseFrame | only "Character Stuck" matters, already an option in `death` / `surroundings` |
| Blizzard_GMChatUI, Blizzard_GMSurveyUI | GM tools |
| VoiceChat, BNet, BNConversations | not on a 3.3.5a private server |
| Blizzard_MacroUI | bots act through commands, not macros |
| Blizzard_TimeManager (stopwatch, alarm) | the clock is in `minimap`; stopwatch is a player tool |
| DressUpFrame, Blizzard_AuctionDressUp | appearance preview; looks are not modeled |
| TutorialFrame | new-player tutorials; bots learn from `RULES.md` / experience |
| Blizzard_DebugTools, MovieRecordingProgress, RatingMenuFrame, TicTacToeFrame, MinigameFrame | debug / legal / unused |

### Widget (no information of its own)

AnimTimerFrame, AutoComplete, CoinPickupFrame, FloatingChatFrame, HybridScrollFrame,
MoneyFrame, MoneyInputFrame, PetPopup, SparkleFrame, StackSplitFrame, UIMenu, UnitPopup.
(StackSplitFrame and UnitPopup are input; they become commands, not snapshot fields.)
