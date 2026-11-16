# Audit: what mod-playerbots gives "random bots" that our bots won't have

Context: we replace playerbots' random-bot manager with our own bot manager (route A,
decided 2026-11-15). Our manager (`server_module/mod-llm-brain/src/bot_manager.cc`) decides
who is online and logs them in with playerbots' public `AddPlayerBot(guid, 0)` (no master).
Playerbots then runs them through its own plumbing: a login without a master always ends in
`RandomPlayerbotMgr`'s holder (sessions drained and AI ticked by playerbots' world / player
scripts, independent of `RandomBotAutologin`). With `RandomBotAutologin = 0` the manager
itself does nothing else: no picking, rotation, refresh, revive or teleports. Our bots are
**not** random bots (`IsRandomBot()` is false: the manager's private `currentBots` set is
empty).

The manager's **login hook** (`RandomPlayerbotMgr::OnBotLoginInternal`) still runs for our
bots: it would put them in automatic bot guilds (`RandomBotGuildCount`) and, from level 70,
bot arena teams (`RandomBotArenaTeam*Count`). Both are set to 0 in our config. It also adds
non-random bots to its list of "real players" (only used by the activity throttle, which is
off: `BotActiveAlone = 100`). This audit lists every place playerbots
checks `IsRandomBot` (89 call sites in 71 functions, pinned submodule as of 2026-11-15) and
what that means for our bots. No edits to `third_party/`.

## Summary

| Category | Sites | Effect on our bots | Action |
|---|---|---|---|
| A. Manager internals, dead code, admin listing | 26 | none (only run for the manager's own bots, or commented out) | none |
| B. Cheats and automatic maintenance | 17 | **off**, which is what we want (no free gear, talents, spells, items, food, teleports, XP rate) | none |
| C. Strategy setup | 16 | our bots get the "companion" default set (with `follow`, without `grind` / `new rpg`), and playerbots **resets** to it after many events | our manager applies its strategy set at login **and re-applies it after every reset** |
| D. Decisions that belong to the brain | 30 | off: invites from strangers, Dungeon Finder / battleground queues, trading, group leadership, LFG chat | brain options + commands (phase 2+); fine for a solo phase-2 bot |
| E. Real gaps (needs a playerbots patch) | 0 | - | none found |

Everything in B stays off on purpose. The one config-controlled item,
`Playerbots.AutoEquipUpgradeLoot` (equip looted upgrades), stays `0` as already decided:
choosing gear is the bot's decision (the brain's), not the body's.

## The one design item: strategy resets

`PlayerbotAI::ResetStrategies()` rebuilds every engine from `AiFactory`'s defaults. For a
non-random bot without a master those are the companion defaults: non-combat
`nc, food, chat, follow, default, force rebuff, quest, loot, gather, duel, pvp, buff, mount,
emote` (+ class strategies), no `grind`, no `new rpg`; combat without the solo `boost` /
`dps assist` / spec-dps strategies. Playerbots calls it on login (`OnBotLogin`, then
`PlayerbotRepository::Load` restores saved strategies) and after: accepting an invite,
passing leadership, `reset` commands, leaving a battleground / arena, Dungeon Finder accept.

So our manager owns a per-bot strategy set (config, later per personality) and:
1. applies it after login (after `PlayerbotRepository::Load`);
2. re-applies it whenever it was reset: a cheap per-tick check on the world thread
   (`HasStrategy("grind", BOT_STATE_NON_COMBAT)` false while our set includes it), no hooks
   into playerbots needed.

Strategies to apply for parity with random bots (from `AiFactory`, solo, not in a BG):
- non-combat: `-follow`, `+grind`, `+new rpg` (questing), plus `+start duel` for 25% of bots
  if wanted; **not** `lfg` / `bg` (queueing is a brain decision);
- combat (solo): `+boost`, `+dps assist`, `-threat`, and the spec-dps extras for healers
  (priest non-shadow: `holy dps, shadow debuff, shadow aoe`; resto druid: `aoe`; resto shaman:
  `ele, caster aoe`; holy paladin: `ret, baoe`);
- dead: `-follow` (a random bot without a group doesn't follow when dead).

## Sites by category

### A. Manager internals, dead code, admin listing (26)
| Where | What |
|---|---|
| `RandomPlayerbotMgr::IsRandomBot` (3) | the check itself |
| `RandomPlayerbotMgr::IsRemovableBot` (2), `RemoveRandomBots`, `ProcessBot` (2), `CheckBgQueue`, `OnPlayerLogin`, `HandlePlayerbotConsoleCommand` | the manager's own bots only; it runs none (autologin off) |
| `RandomBotLevelMgr::GetOrFlagPlayerBracket`, `RunLevelBracketsDistribution`, `ProcessPendingLevelResets`, `RunResetPlayedTimeCheck`, `OnBotLogin`, `OnBotLevelChanged` | level brackets / level resets for random bots (off in our config anyway) |
| `RandomBotUpdateAction::Execute` | random-bot periodic maintenance (leave bot-led groups etc.) |
| `GuildTaskMgr::CheckItemTask`, `CheckTaskTransfer` | guild tasks handed to random bots |
| `PlayerbotHolder::ListBots` | `.playerbots bot list` output |
| `PlayerbotHolder::ProcessBotCommand`, `ChangeTalentsAction::SpecApply` (2), `XpGainAction::Execute`, `GrindTargetValue::FindTargetForGrinding` | commented-out code |

### B. Cheats and automatic maintenance (17): off for our bots, as intended
| Where | What random bots get |
|---|---|
| `AutoMaintenanceOnLevelupAction::AutoTeleportForLevel` | teleport to a level-appropriate zone on level-up |
| `AutoMaintenanceOnLevelupAction::AutoPickTalents`, `AutoSetTalentsAction::Execute` | talents picked automatically |
| `AutoMaintenanceOnLevelupAction::LearnSpells` (2) | trainer and quest spells learned for free on level-up |
| `AutoMaintenanceOnLevelupAction::AutoUpgradeEquip` | gear generated on level-up |
| `PlayerbotFactory::InitTradeSkills`, `IsTrainerSpellAllowedForBot` | professions assigned at creation |
| `EquipFishingPoleAction::isUseful` | a fishing pole created from nothing |
| `NoFoodTrigger`, `NoDrinkTrigger`, `GiveFoodAction`, `GiveWaterAction` | the food/drink cheat |
| `OnPlayerGiveXP` (`Script/Playerbots.cpp`) | `RandomBotXPRate` multiplier (normal XP for ours) |
| `OnPlayerBeforeAchievementComplete` (`Script/Playerbots.cpp`) | excluded from realm-first achievements (ours can earn them, like players) |
| `PlayerbotHolder::LogoutPlayerBot` | taxi-cheat flag cleared at logout for non-random bots (harmless) |
| `EquipUpgradesPacketAction::Execute` | equip looted upgrades; ours only with `AutoEquipUpgradeLoot = 1`, which stays `0` (gear is the brain's choice) |

### C. Strategy setup (16): handled by our manager (see above)
| Where | What |
|---|---|
| `PlayerbotHolder::OnBotLogin` (2) | strategy reset at login; random bots ≥ level 10 join the custom "World" chat channel (not wanted: not a 3.3.5a channel) |
| `AiFactory::AddDefaultCombatStrategies` (2) | solo combat extras; `RandomBotCombatStrategies` vs `CombatStrategies` config |
| `AiFactory::AddDefaultNonCombatStrategies` (2) | `grind`, `new rpg`, `start duel`, `lfg`, `bg`; `RandomBotNonCombatStrategies` vs `NonCombatStrategies` |
| `AiFactory::AddDefaultDeadStrategies` | no `follow` while dead |
| `AiFactory::AddDefaultReactionStrategies` | `RandomBotReactStrategies` vs `ReactStrategies` (both empty in our config) |
| `ArenaTactics::Execute` (2), `BGLeaveAction::Execute` (3), `BGStatusAction::LeaveBG`, `BGStatusAction::Execute` (2) | strategy reset after battlegrounds / arenas; random bots also drop their master |
| `LfgAcceptAction::Execute` (counted in D) | reset after a Dungeon Finder group |

### D. Decisions that belong to the brain (30): off until the brain makes them
| Where | What random bots do on their own |
|---|---|
| `AcceptInvitationAction::Execute` | make the inviter their master (acceptance itself is gated by playerbots' security level, which declines strangers for our bots) |
| `SecurityCheckAction::isUseful`, `PlayerbotAI::IsTellAllowed`, `PlayerbotAI::IsAltBot` | master / whisper rules for random bots |
| `PlayerbotAI::UpdateAIGroupMaster` (2), `PartyCommandAction::Execute`, `PassLeadershipToMasterAction::Execute`, `ResetAiAction::Execute` | pick / drop masters, leave or lead groups |
| `LfgJoinAction::isUseful`, `GetRoles`, `LfgAcceptAction::Execute` (2), `LfgLeaveAction::Execute` (2) | queue for the Dungeon Finder, accept, leave |
| `BGStatusAction` / `BGLeaveAction` master handling | (counted in C) |
| `InviteNearbyToGroupAction::Execute`, `InviteGuildToGroupAction::Execute` | invite chat lines |
| `GuildManageNearbyAction::Execute` (2) | guild invites and chat |
| `TradeStatusAction::Execute`, `BeginTrade`, `CheckTrade` (2) | trading rules (`EnableRandomBotTrading`), discounts |
| `WhoAction::Execute`, `WtsAction::Execute`, `QueryItemUsageAction::QueryItemPrice`, `StoreLootAction::Execute` | price talk, "WTS" replies, selling loot to a master |
| `HireAction::Execute` | being hired by a player as an alt |
| `SuggestWhatToDoAction::isUseful` | random "LFG / let's do X" suggestions in chat |
| `TrainerAction::Execute` | learn everything at a trainer; ours learn only with an explicit `learn` (the brain's `train` goal) |

## The second check: random-bot accounts (found 2026-11-15, to audit)

Besides `IsRandomBot` (by character), playerbots also tells random bots apart by **account**:
`PlayerbotAIConfig::IsInRandomAccountList`, at ~17 live sites. Our bots are on our own
accounts, so every one of these treats them as real players' characters. This audit didn't
cover them, and one already broke the server:

- **Chat storm (fixed):** `PlayerbotAI::HandleBotOutgoingPacket` rate-limits replies only to
  bots on random accounts ("isFromFreeBot"). 50 of our bots in General - Durotar answered each
  other without a cooldown, in a loop inside one world update, until the world thread hung at
  ~20 GB. Fix: `Playerbots.RandomBotTalk = 0` (`config/playerbots.overrides`); what a bot says
  is its brain's call anyway.
- **To review:** `PlayerbotSecurity.cpp:50` (who may command the bot), `TrainerAction.cpp:323,
  579`, `TradeStatusAction.cpp:219`, `SendMailAction.cpp:18`, `CheckMailAction.cpp:32`,
  `BattleGroundJoinAction.cpp:105`, `ChangeStrategyAction.cpp:51`, `PlayerbotGuildMgr.cpp:217,
  253`, `PlayerbotMgr.cpp:551, 632`, `RandomPlayerbotMgr.cpp:2491`,
  `RandomPlayerbotFactory.cpp:858, 921`. Re-run with `grep -rn IsInRandomAccountList` when
  bumping the submodule.

## Risks to watch in the parity test

- A non-random bot **without a master** is less-travelled in playerbots: random bots have no
  master, alts have one. Code that assumes "non-random implies master" could misbehave; the
  parity run (phase-1 solo baseline: level 1 to 5 in 59 min, 6 quests) is where it shows.
- `PlayerbotRepository::Load` restores strategies saved in `acore_playerbots` at the bot's
  last change; for characters that were random bots before, those may be stale. Our manager
  applies its set after it.
- Upstream changes: these are internal classes, not a stable API. The submodule is pinned;
  re-run this audit (`grep -rn IsRandomBot`) when bumping it.
