# Snapshot coverage: the client's UI frames

Audit of `snapshot.md` (rev 6) against every UI frame the 3.3.5a client ships
(`Interface/FrameXML` and `Interface/AddOns/Blizzard_*` from the MPQs: 179 XML files, 149
after dropping templates, fonts and helpers). Each frame shows something a player can see;
the snapshot must have a place for it, or the frame must be marked out of scope with a
reason.

Status: **covered** (spec location exists), **thin** (exists, missing fields), **gap**
(no place yet), **out** (out of scope), **widget** (no game information of its own).

## Covered

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

## Thin (exists, missing fields)

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

## Gap (no place yet)

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

## Out of scope

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

## Widget (no information of its own)

AnimTimerFrame, AutoComplete, CoinPickupFrame, FloatingChatFrame, HybridScrollFrame,
MoneyFrame, MoneyInputFrame, PetPopup, SparkleFrame, StackSplitFrame, UIMenu, UnitPopup.
(StackSplitFrame and UnitPopup are input; they become commands, not snapshot fields.)
