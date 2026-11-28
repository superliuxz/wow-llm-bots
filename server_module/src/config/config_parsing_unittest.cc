// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "config/config_parsing.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bot_manager/tick_owner.h"
#include "bot_session.h"
#include "gtest/gtest.h"

namespace llm_brain {
namespace {

using Strings = std::vector<std::string>;
using Fixtures = std::vector<std::pair<std::string, std::string>>;

TEST(ConfigParsingTest, SplitListTrimsAndDropsEmpty) {
  EXPECT_EQ(SplitList("Dezlu, Wozkaruy,,\tGatda "),
            (Strings{"Dezlu", "Wozkaruy", "Gatda"}));
  EXPECT_EQ(SplitList(""), Strings{});
  EXPECT_EQ(SplitList(" , "), Strings{});
}

TEST(ConfigParsingTest, ParseFixturesJoinsRelativeFilesToDir) {
  EXPECT_EQ(ParseFixtures("Testpriest=priest.pdump, Testwar=/abs/war.pdump",
                          "/repo/server_module/testdata/characters"),
            (Fixtures{{"Testpriest",
                       "/repo/server_module/testdata/characters/priest.pdump"},
                      {"Testwar", "/abs/war.pdump"}}));
  EXPECT_EQ(ParseFixtures("Testpriest=priest.pdump", ""),
            (Fixtures{{"Testpriest", "priest.pdump"}}));
  EXPECT_EQ(ParseFixtures("Loada=new:orc:warrior",
                          "/repo/server_module/testdata/characters"),
            (Fixtures{{"Loada", "new:orc:warrior"}}));
}

TEST(ConfigParsingTest, ParseFixturesSkipsMalformedEntries) {
  EXPECT_EQ(ParseFixtures("NoFile, =x.pdump, Name=, Ok=ok.pdump", ""),
            (Fixtures{{"Ok", "ok.pdump"}}));
}

TEST(ConfigParsingTest, StrategyChecksCoverEveryChange) {
  using Checks = std::vector<StrategyCheck>;
  EXPECT_EQ(StrategyChecks("-follow,+grind,+new rpg"),
            (Checks{{"follow", false}, {"grind", true}, {"new rpg", true}}));
  // Turning grinding off: "grind absent" is checked too, not only "follow".
  EXPECT_EQ(StrategyChecks("-grind, -new rpg, -follow"),
            (Checks{{"grind", false}, {"new rpg", false}, {"follow", false}}));
  EXPECT_EQ(StrategyChecks("+stay,-stay"), (Checks{{"stay", false}}));
  EXPECT_EQ(StrategyChecks("-follow,-stay"),
            (Checks{{"follow", false}, {"stay", false}}));
  EXPECT_EQ(StrategyChecks(""), Checks{});
  EXPECT_EQ(StrategyChecks("grind, +"), Checks{});
}

TEST(ConfigParsingTest, AdjustNonCombatStrategiesRespectsGroupAndDungeon) {
  const std::string live = "-follow,+grind,+new rpg";
  // Brain-led (harness connected & LLM healthy): solo bots and all group
  // leaders strip both `grind` and `new rpg` so playerbots never accepts/turns
  // in/abandons quests, picks rewards, flies, or teleports alongside the brain.
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/false,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/false,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/true),
            "-follow,-grind,-new rpg");
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/false,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/true,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/true),
            "-follow,-grind,-new rpg");
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/true,
                                      /*in_dungeon=*/false,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/true),
            "-follow,-grind,-new rpg");
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/true,
                                      /*in_dungeon=*/true,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/true),
            "-follow,-grind,-new rpg");
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/false,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/true),
            "+follow,-grind,-new rpg");
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/true,
                                      /*group_has_human=*/true,
                                      /*brain_led=*/true),
            "+follow,-grind,-new rpg");

  // Playerbots-led fallback (harness disconnected or LLM unhealthy):
  // Solo open-world: unchanged so the bot stays active.
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/false,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/false,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/false),
            live);
  // Solo in dungeon: strips +new rpg so it does not hearthstone out.
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/false,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/true,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/false),
            "-follow,+grind,-new rpg");
  // All-bot group leader in open world: unchanged.
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/true,
                                      /*in_dungeon=*/false,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/false),
            live);
  // All-bot group leader in dungeon: keeps +grind, strips +new rpg.
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/true,
                                      /*in_dungeon=*/true,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/false),
            "-follow,+grind,-new rpg");
  // Grouped follower (open-world or dungeon): runs with leader (+follow).
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/true,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/false),
            "+follow,-grind,-new rpg");
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/false,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/false),
            "+follow,-grind,-new rpg");
  // Adversarial counter-test: a bot leading a human NEVER solo-grinds or
  // RPG-travels, even when playerbots-led (harness disconnected).
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/true,
                                      /*in_dungeon=*/false,
                                      /*group_has_human=*/true,
                                      /*brain_led=*/false),
            "-follow,-grind,-new rpg");
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/true,
                                      /*in_dungeon=*/true,
                                      /*group_has_human=*/true,
                                      /*brain_led=*/false),
            "-follow,-grind,-new rpg");
  // Config with only `+new rpg` (no `+grind`) is still stripped when brain-led.
  EXPECT_EQ(AdjustNonCombatStrategies("-follow,-grind,+new rpg",
                                      /*in_group=*/false,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/false,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/true),
            "-follow,-grind,-new rpg");
  // Stripped test configs (neither `+grind` nor `+new rpg`) are never altered.
  const std::string boss = "-follow,-grind,-new rpg,-wotlk-hol";
  EXPECT_EQ(AdjustNonCombatStrategies(boss, /*in_group=*/true,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/true,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/true),
            boss);
  EXPECT_EQ(AdjustNonCombatStrategies(boss, /*in_group=*/false,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/false,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/false),
            boss);
}

// Adversarial counter-test: human join/leave, harness disconnect/reconnect,
// and logout/reset transitions for a solo bot.
TEST(ConfigParsingTest,
     AdjustNonCombatStrategiesHumanJoinLeaveAndLogoutTransitions) {
  const std::string live = "-follow,+grind,+new rpg";
  BotSessionRegistry reg;
  constexpr uint64_t kBot = 0x2001;

  // 1. Solo brain-led: no grind / new rpg overlap.
  reg.WithSession(kBot,
                  [](BotSession& s) { s.tick_owner = TickOwner::kBrain; });
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/false,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/false,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/true),
            "-follow,-grind,-new rpg");

  // 2. Human invites bot (bot becomes follower in human group).
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/false,
                                      /*group_has_human=*/true,
                                      /*brain_led=*/true),
            "+follow,-grind,-new rpg");

  // 3. Human promotes bot to leader (still no follow, no grind, no new rpg).
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/true,
                                      /*is_leader=*/true,
                                      /*in_dungeon=*/false,
                                      /*group_has_human=*/true,
                                      /*brain_led=*/true),
            "-follow,-grind,-new rpg");

  // 4. Human leaves group -> bot is solo again, still brain-led: MUST stay
  //    `-follow,-grind,-new rpg` (never re-enables playerbots new rpg/grind).
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/false,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/false,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/true),
            "-follow,-grind,-new rpg");

  // 5. Logout resets tick_owner to kPlayerbots; until brain reconnects, solo
  //    bot falls back cleanly to playerbots' open-world strategies.
  reg.ResetOnLogout(kBot);
  const bool brain_after_logout =
      reg.InspectSession(kBot, [](const BotSession* s) {
        return s && s->tick_owner == TickOwner::kBrain;
      });
  EXPECT_FALSE(brain_after_logout);
  EXPECT_EQ(AdjustNonCombatStrategies(live, /*in_group=*/false,
                                      /*is_leader=*/false,
                                      /*in_dungeon=*/false,
                                      /*group_has_human=*/false,
                                      /*brain_led=*/brain_after_logout),
            live);
}

TEST(ConfigParsingTest, ScriptedRaidStrategyForMapCoversRaidsAndExcludesRest) {
  // WotLK raids (and classic/TBC raids in RaidStrategyContext).
  EXPECT_EQ(ScriptedRaidStrategyForMap(533), "naxx");
  EXPECT_EQ(ScriptedRaidStrategyForMap(603), "ulduar");
  EXPECT_EQ(ScriptedRaidStrategyForMap(631), "icc");
  EXPECT_EQ(ScriptedRaidStrategyForMap(249), "onyxia");
  EXPECT_EQ(ScriptedRaidStrategyForMap(615), "wotlk-os");
  EXPECT_EQ(ScriptedRaidStrategyForMap(616), "wotlk-eoe");
  EXPECT_EQ(ScriptedRaidStrategyForMap(624), "voa");
  EXPECT_EQ(ScriptedRaidStrategyForMap(724), "rs");
  EXPECT_EQ(ScriptedRaidStrategyForMap(409), "moltencore");
  EXPECT_EQ(ScriptedRaidStrategyForMap(532), "karazhan");
  EXPECT_EQ(ScriptedRaidStrategyForMap(564), "blacktemple");

  // Adversarial counter-tests: continents, 5-man dungeons, and battlegrounds
  // are NOT scripted raid strategies.
  for (uint32_t non_raid :
       {0u, 1u, 30u, 36u, 489u, 530u, 571u, 574u, 602u, 650u}) {
    EXPECT_TRUE(ScriptedRaidStrategyForMap(non_raid).empty()) << non_raid;
  }
}

TEST(ConfigParsingTest, AdjustRaidStrategiesCoordinatesBrainAndFallback) {
  const std::string combat = "+boost,+dps assist,+threat";
  const std::string non_combat = "+follow,-grind,-new rpg";
  const std::string raw_non_combat = "-follow,+grind,+new rpg";

  // Brain-led in WotLK raids (Naxx 533, Ulduar 603, OS 615, EoE 616, VoA 624,
  // ICC 631, RS 724): preserves playerbots' raid strategy as the body layer
  // (vehicles, harpoons, cannons, Malygos drakes, boss positioning) instead of
  // blanket-stripping it.
  EXPECT_EQ(AdjustRaidStrategies(combat, 533, /*brain_led=*/true),
            "+boost,+dps assist,+threat,+naxx");
  EXPECT_EQ(AdjustRaidStrategies(non_combat, 533, /*brain_led=*/true),
            "+follow,-grind,-new rpg,+naxx");
  EXPECT_EQ(AdjustRaidStrategies(combat, 603, /*brain_led=*/true),
            "+boost,+dps assist,+threat,+ulduar");
  EXPECT_EQ(AdjustRaidStrategies(combat, 615, /*brain_led=*/true),
            "+boost,+dps assist,+threat,+wotlk-os");
  EXPECT_EQ(AdjustRaidStrategies(combat, 616, /*brain_led=*/true),
            "+boost,+dps assist,+threat,+wotlk-eoe");
  EXPECT_EQ(AdjustRaidStrategies(combat, 624, /*brain_led=*/true),
            "+boost,+dps assist,+threat,+voa");
  EXPECT_EQ(AdjustRaidStrategies(combat, 631, /*brain_led=*/true),
            "+boost,+dps assist,+threat,+icc");
  EXPECT_EQ(AdjustRaidStrategies(combat, 724, /*brain_led=*/true),
            "+boost,+dps assist,+threat,+rs");

  // Strips only clashing autonomous open-world sub-behaviors (`grind`,
  // `new rpg`) when brain-led, while keeping the raid strategy on.
  EXPECT_EQ(AdjustRaidStrategies(raw_non_combat, 631, /*brain_led=*/true),
            "-follow,-grind,-new rpg,+icc");
  // When playerbots-led (disconnect fallback), `grind`/`new rpg` are not
  // stripped by AdjustRaidStrategies while `+icc` remains enabled.
  EXPECT_EQ(AdjustRaidStrategies(raw_non_combat, 631, /*brain_led=*/false,
                                 /*apply_instance_strategies=*/true),
            "-follow,+grind,+new rpg,+icc");
  EXPECT_EQ(AdjustRaidStrategies(combat, 631, /*brain_led=*/false,
                                 /*apply_instance_strategies=*/true),
            "+boost,+dps assist,+threat,+icc");
  EXPECT_EQ(AdjustRaidStrategies(non_combat, 631, /*brain_led=*/false,
                                 /*apply_instance_strategies=*/true),
            "+follow,-grind,-new rpg,+icc");

  // Adversarial counter-test: if `Playerbots.ApplyInstanceStrategies = 0`,
  // neither brain-led nor disconnect fallback forces `+icc` on (while brain-led
  // still strips clashing `+grind`/`+new rpg` if present).
  EXPECT_EQ(AdjustRaidStrategies(combat, 631, /*brain_led=*/true,
                                 /*apply_instance_strategies=*/false),
            combat);
  EXPECT_EQ(AdjustRaidStrategies(combat, 631, /*brain_led=*/false,
                                 /*apply_instance_strategies=*/false),
            combat);
  EXPECT_EQ(AdjustRaidStrategies(raw_non_combat, 631, /*brain_led=*/true,
                                 /*apply_instance_strategies=*/false),
            "-follow,-grind,-new rpg");

  // Adversarial counter-test: if a config/benchmark explicitly disabled `-icc`,
  // both brain-led and disconnect fallback respect `-icc` rather than forcing
  // `+icc` back on.
  const std::string explicit_off = "+boost,+dps assist,-threat,-icc";
  EXPECT_EQ(AdjustRaidStrategies(explicit_off, 631, /*brain_led=*/true,
                                 /*apply_instance_strategies=*/true),
            explicit_off);
  EXPECT_EQ(AdjustRaidStrategies(explicit_off, 631, /*brain_led=*/false,
                                 /*apply_instance_strategies=*/true),
            explicit_off);
  // Idempotent when already `+icc` (no duplicate `+icc`).
  const std::string explicit_on = "+boost,+dps assist,+threat,+icc";
  EXPECT_EQ(AdjustRaidStrategies(explicit_on, 631, /*brain_led=*/true,
                                 /*apply_instance_strategies=*/true),
            explicit_on);

  // Adversarial counter-test: 5-man dungeon (HoL 602) and open world (571)
  // are completely untouched in both modes.
  EXPECT_EQ(AdjustRaidStrategies(combat, 602, /*brain_led=*/true), combat);
  EXPECT_EQ(AdjustRaidStrategies(combat, 602, /*brain_led=*/false), combat);
  EXPECT_EQ(AdjustRaidStrategies(combat, 571, /*brain_led=*/true), combat);
  EXPECT_EQ(AdjustRaidStrategies(combat, 571, /*brain_led=*/false), combat);
  EXPECT_EQ(AdjustRaidStrategies(raw_non_combat, 571, /*brain_led=*/true),
            raw_non_combat);

  // Map-change & disconnect/reconnect transition: ICC (brain-led -> `+icc`) ->
  // Dalaran (571 -> clean) -> Ulduar (brain-led -> `+ulduar`) -> disconnect
  // (`+ulduar`) -> reconnect (`+ulduar`).
  EXPECT_EQ(AdjustRaidStrategies(combat, 631, /*brain_led=*/true),
            "+boost,+dps assist,+threat,+icc");
  EXPECT_EQ(AdjustRaidStrategies(combat, 571, /*brain_led=*/true), combat);
  EXPECT_EQ(AdjustRaidStrategies(combat, 603, /*brain_led=*/true),
            "+boost,+dps assist,+threat,+ulduar");
  EXPECT_EQ(AdjustRaidStrategies(combat, 603, /*brain_led=*/false),
            "+boost,+dps assist,+threat,+ulduar");
  EXPECT_EQ(AdjustRaidStrategies(combat, 603, /*brain_led=*/true),
            "+boost,+dps assist,+threat,+ulduar");
}

TEST(ConfigParsingTest, GroupedCombatStrategiesPreservesBoostAndThreatRules) {
  const std::string base = "+boost,+dps assist,-threat";
  // Grouped DPS keep `boost` (for bosses / heavy pulls) and enable `+threat`.
  EXPECT_EQ(GroupedCombatStrategies(base, /*is_healer=*/false),
            "+boost,+dps assist,+threat");
  // Grouped healers keep `boost` (e.g. Bloodlust/Heroism, Shadowfiend) but MUST
  // keep `-threat`, or playerbots' ThreatMultiplier blocks all
  // CastHealingSpellAction spells at >= 50% AoE threat.
  EXPECT_EQ(GroupedCombatStrategies(base, /*is_healer=*/true),
            "+boost,+dps assist,-threat");
  // Adversarial counter-test: even if a config passed `+threat`, healers still
  // get `-threat`, and unrelated strategies stay intact.
  EXPECT_EQ(GroupedCombatStrategies("+boost,+threat,-wotlk-hol",
                                    /*is_healer=*/true),
            "+boost,-threat,-wotlk-hol");
  // Adversarial counter-test: if a config explicitly disables `boost`, grouped
  // mode preserves `-boost` rather than forcing it on.
  EXPECT_EQ(GroupedCombatStrategies("-boost,+dps assist,-threat",
                                    /*is_healer=*/false),
            "-boost,+dps assist,+threat");
  EXPECT_EQ(GroupedCombatStrategies("-boost,+dps assist,-threat",
                                    /*is_healer=*/true),
            "-boost,+dps assist,-threat");
  // Adversarial counter-test: solo combat strategies (not passed through
  // GroupedCombatStrategies) keep `-threat` and `+boost` untouched.
  using Checks = std::vector<StrategyCheck>;
  EXPECT_EQ(StrategyChecks(base),
            (Checks{{"boost", true}, {"dps assist", true}, {"threat", false}}));
}

TEST(ConfigParsingTest, GroupReflexLimitsHoldBackBodyPulls) {
  using L = GroupReflexLimits;
  // Solo bots keep every reflex, even in a dungeon (counter-test).
  EXPECT_EQ(GroupReflexLimitsFor(/*in_group=*/false, /*in_dungeon=*/false,
                                 /*group_has_human=*/false),
            L{});
  EXPECT_EQ(GroupReflexLimitsFor(/*in_group=*/false, /*in_dungeon=*/true,
                                 /*group_has_human=*/false),
            L{});
  // An all-bot group outdoors keeps `flee`; its pets still stay defensive.
  EXPECT_EQ(GroupReflexLimitsFor(/*in_group=*/true, /*in_dungeon=*/false,
                                 /*group_has_human=*/false),
            (L{.block_flee = false, .pet_defensive = true}));
  // In a dungeon or with a human: no `flee`.
  EXPECT_EQ(GroupReflexLimitsFor(/*in_group=*/true, /*in_dungeon=*/true,
                                 /*group_has_human=*/false),
            (L{.block_flee = true, .pet_defensive = true}));
  EXPECT_EQ(GroupReflexLimitsFor(/*in_group=*/true, /*in_dungeon=*/false,
                                 /*group_has_human=*/true),
            (L{.block_flee = true, .pet_defensive = true}));
}

}  // namespace
}  // namespace llm_brain
