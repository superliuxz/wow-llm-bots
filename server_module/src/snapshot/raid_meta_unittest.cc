// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/raid_meta.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "gtest/gtest.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

TEST(RaidMetaTest, BuildInstanceMetaPopulatesAllDifficultiesAndLockout) {
  auto make = [](bool is_raid, uint8_t mode, uint32_t max_players,
                 bool perm_bound = false, int64_t rem_s = 0) {
    InstanceMetaInput in;
    in.is_dungeon_or_raid = true;
    in.is_raid = is_raid;
    in.spawn_mode = mode;
    in.max_players = max_players;
    in.map_name = is_raid ? "Icecrown Citadel" : "Utgarde Pinnacle";
    in.perm_bound = perm_bound;
    in.now_epoch_s = 1000000;
    in.reset_time_epoch_s = perm_bound ? (1000000 + rem_s) : 0;
    return BuildInstanceMeta(in);
  };

  auto d5n = make(false, kAcoreDungeonNormal, 5);
  ASSERT_TRUE(d5n.has_value());
  EXPECT_EQ(d5n->difficulty, "5N");
  EXPECT_EQ(d5n->size, 5u);
  EXPECT_TRUE(d5n->locked_until.empty());

  auto d5h = make(false, kAcoreDungeonHeroic, 5, /*perm_bound=*/true,
                  5 * 3600 + 20 * 60);
  ASSERT_TRUE(d5h.has_value());
  EXPECT_EQ(d5h->difficulty, "5H");
  EXPECT_EQ(d5h->size, 5u);
  EXPECT_EQ(d5h->locked_until, "locked (5h 20m)");

  auto r10n = make(true, kAcoreRaid10Normal, 10);
  ASSERT_TRUE(r10n.has_value());
  EXPECT_EQ(r10n->difficulty, "10N");
  EXPECT_EQ(r10n->size, 10u);

  auto r10h = make(true, kAcoreRaid10Heroic, 10);
  ASSERT_TRUE(r10h.has_value());
  EXPECT_EQ(r10h->difficulty, "10H");
  EXPECT_EQ(r10h->size, 10u);

  auto r25n = make(true, kAcoreRaid25Normal, 25);
  ASSERT_TRUE(r25n.has_value());
  EXPECT_EQ(r25n->difficulty, "25N");
  EXPECT_EQ(r25n->size, 25u);

  auto r25h = make(true, kAcoreRaid25Heroic, 25, /*perm_bound=*/true,
                   2 * 86400 + 4 * 3600);
  ASSERT_TRUE(r25h.has_value());
  EXPECT_EQ(r25h->difficulty, "25H");
  EXPECT_EQ(r25h->size, 25u);
  EXPECT_EQ(r25h->name, "Icecrown Citadel");
  EXPECT_EQ(r25h->locked_until, "locked (2d 4h)");

  // Adversarial counter-test: outdoors leaves `ext.instance` empty.
  InstanceMetaInput outdoors;
  outdoors.is_dungeon_or_raid = false;
  outdoors.map_name = "Northrend";
  EXPECT_FALSE(BuildInstanceMeta(outdoors).has_value());
}

TEST(RaidMetaTest, MapAcoreDispelTypeMapsKnownTypesOnly) {
  EXPECT_EQ(MapAcoreDispelType(kAcoreDispelMagic), pb::DispelType::MAGIC);
  EXPECT_EQ(MapAcoreDispelType(kAcoreDispelCurse), pb::DispelType::CURSE);
  EXPECT_EQ(MapAcoreDispelType(kAcoreDispelDisease), pb::DispelType::DISEASE);
  EXPECT_EQ(MapAcoreDispelType(kAcoreDispelPoison), pb::DispelType::POISON);
  EXPECT_EQ(MapAcoreDispelType(0), pb::DispelType::DISPEL_TYPE_UNSPECIFIED);
  EXPECT_EQ(MapAcoreDispelType(99), pb::DispelType::DISPEL_TYPE_UNSPECIFIED);
}

TEST(RaidMetaTest, ComputeMissingRaidBuffsDetectsMissingAndPrefersGroupSpell) {
  const std::vector<std::string> priest_spells = {
      "Power Word: Fortitude", "Prayer of Fortitude", "Divine Spirit",
      "Shadow Protection"};
  const std::vector<std::vector<std::string>> party_auras = {
      {"Prayer of Fortitude", "Divine Spirit", "Shadow Protection"},
      {"Power Word: Fortitude", "Shadow Protection"},  // Missing Spirit.
  };
  const std::vector<std::string> missing =
      ComputeMissingRaidBuffs(priest_spells, party_auras);
  EXPECT_EQ(missing, (std::vector<std::string>{"Divine Spirit"}));

  // If someone is missing Fortitude, prefers the group version when known.
  const std::vector<std::vector<std::string>> missing_fort = {
      {"Divine Spirit", "Shadow Protection"},
  };
  EXPECT_EQ(ComputeMissingRaidBuffs(priest_spells, missing_fort),
            (std::vector<std::string>{"Prayer of Fortitude"}));

  // Alternative aura (Dalaran Brilliance) satisfies Arcane Intellect family.
  EXPECT_TRUE(
      ComputeMissingRaidBuffs({"Arcane Brilliance"}, {{"Dalaran Brilliance"}})
          .empty());

  // Counter-test: empty spellbook or empty member list returns empty.
  EXPECT_TRUE(ComputeMissingRaidBuffs({}, party_auras).empty());
  EXPECT_TRUE(ComputeMissingRaidBuffs(priest_spells, {}).empty());
}

TEST(RaidMetaTest, BuildDbmWarningFromBossChatReplacesSenderAndTrims) {
  auto w = BuildDbmWarningFromBossChat("The Lich King",
                                       "  %s begins to cast Defile!  ");
  ASSERT_TRUE(w.has_value());
  EXPECT_EQ(w->text, "The Lich King begins to cast Defile!");

  EXPECT_FALSE(BuildDbmWarningFromBossChat("Boss", "   ").has_value());
}

TEST(RaidMetaTest, BossTimerTrackerTracksCastsLearnsIntervalsAndResets) {
  BossTimerTracker tracker;
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);

  // First cast of Defile (2.0s cast time).
  tracker.Update(/*boss_guid=*/0xB055, /*boss_in_combat=*/true,
                 BossTimerTracker::ActiveCast{"Defile", 2.0f}, t0);
  auto timers = tracker.GetTimers(t0);
  ASSERT_EQ(timers.size(), 1u);
  EXPECT_EQ(timers[0].name, "Defile");
  EXPECT_NEAR(timers[0].remaining_s, 2.0f, 1e-2f);

  // Second cast of Defile 30s later: learns 30s interval and projects Next.
  const base::TimeTicks t1 = t0 + base::Seconds(30);
  tracker.Update(0xB055, true, BossTimerTracker::ActiveCast{"Defile", 2.0f},
                 t1);
  timers = tracker.GetTimers(t1);
  ASSERT_EQ(timers.size(), 2u);
  EXPECT_EQ(timers[0].name, "Defile");
  EXPECT_NEAR(timers[0].remaining_s, 2.0f, 1e-2f);
  EXPECT_EQ(timers[1].name, "Next Defile");
  EXPECT_NEAR(timers[1].remaining_s, 30.0f, 1e-2f);

  // Adversarial counter-test 1: leaving combat clears all timers.
  tracker.Update(0xB055, /*boss_in_combat=*/false, std::nullopt,
                 t1 + base::Seconds(5));
  EXPECT_TRUE(tracker.GetTimers(t1 + base::Seconds(5)).empty());

  // Adversarial counter-test 2: switching boss GUID clears previous boss
  // history, and explicit Reset() clears on map change/logout/disconnect.
  tracker.Update(0xB055, true, BossTimerTracker::ActiveCast{"Infest", 2.0f},
                 t1 + base::Seconds(10));
  tracker.Update(0xB056, true, std::nullopt, t1 + base::Seconds(11));
  EXPECT_TRUE(tracker.GetTimers(t1 + base::Seconds(11)).empty());

  tracker.Update(0xB056, true,
                 BossTimerTracker::ActiveCast{"Soul Reaper", 1.5f},
                 t1 + base::Seconds(12));
  ASSERT_EQ(tracker.GetTimers(t1 + base::Seconds(12)).size(), 1u);
  tracker.Reset();
  EXPECT_TRUE(tracker.GetTimers(t1 + base::Seconds(12)).empty());
}

}  // namespace
}  // namespace llm_brain
