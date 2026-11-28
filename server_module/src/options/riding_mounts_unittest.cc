// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/riding_mounts.h"

#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

TEST(RidingMountsTest, CanonicalRidingSpellNamesAndCosts) {
  EXPECT_EQ(RidingSpellName(kSpellApprenticeRiding), "Apprentice Riding");
  EXPECT_EQ(RidingSpellName(kSpellJourneymanRiding), "Journeyman Riding");
  EXPECT_EQ(RidingSpellName(kSpellExpertRiding), "Expert Riding");
  EXPECT_EQ(RidingSpellName(kSpellArtisanRiding), "Artisan Riding");
  EXPECT_EQ(RidingSpellName(kSpellColdWeatherFlying), "Cold Weather Flying");
  EXPECT_TRUE(RidingSpellName(999999).empty());

  EXPECT_EQ(FormatCopperCost(0), "free");
  EXPECT_EQ(FormatCopperCost(50), "50c");
  EXPECT_EQ(FormatCopperCost(9000), "90s");
  EXPECT_EQ(FormatCopperCost(40000), "4g");
  EXPECT_EQ(FormatCopperCost(36000), "3g 60s");
  EXPECT_EQ(FormatCopperCost(10203), "1g 2s 3c");

  EXPECT_EQ(DiscountedCopperCost(40000, 0.9f), 36000u);
  EXPECT_EQ(DiscountedCopperCost(40000, 1.0f), 40000u);
  EXPECT_EQ(DiscountedCopperCost(40000, 1.5f), 40000u);
  EXPECT_EQ(DiscountedCopperCost(40000, -0.5f), 0u);
  EXPECT_EQ(
      DiscountedCopperCost(40000, std::numeric_limits<float>::quiet_NaN()), 0u);
}

TEST(RidingMountsTest, SelectAffordableRidingTrainingOrdersAndFiltersByGold) {
  const std::vector<RidingTrainingCandidate> candidates = {
      {kSpellJourneymanRiding, "Journeyman Riding", 40, 500000, true},
      {kSpellApprenticeRiding, "", 20, 40000, true},
      {kSpellExpertRiding, "Expert Riding", 60, 2500000, false},
  };

  // Enough for Apprentice (4g), not enough for Journeyman (50g).
  const std::vector<RidingTrainingOption> opts =
      SelectAffordableRidingTraining(candidates, 40000, "Randal Hunter");
  ASSERT_EQ(opts.size(), 1u);
  EXPECT_EQ(opts[0].spell_id, kSpellApprenticeRiding);
  EXPECT_EQ(opts[0].cost_copper, 40000u);
  EXPECT_EQ(opts[0].label, "Train Apprentice Riding (4g) at Randal Hunter");

  // Counter-test: 1 copper short of Apprentice Riding -> nothing offered.
  EXPECT_TRUE(SelectAffordableRidingTraining(candidates, 39999, "Randal Hunter")
                  .empty());

  // Enough for both Apprentice and Journeyman: sorted by required level.
  const std::vector<RidingTrainingOption> both =
      SelectAffordableRidingTraining(candidates, 500000, "Randal Hunter");
  ASSERT_EQ(both.size(), 2u);
  EXPECT_EQ(both[0].spell_id, kSpellApprenticeRiding);
  EXPECT_EQ(both[1].spell_id, kSpellJourneymanRiding);
}

TEST(RidingMountsTest, ValidateRidingTrainRejectsCombatAndInsufficientGold) {
  EXPECT_TRUE(ValidateRidingTrain(/*in_combat=*/false, /*valid_trainer=*/true,
                                  /*spell_found=*/true, /*can_teach=*/true,
                                  40000, 40000)
                  .empty());
  // Adversarial counter-tests: combat priority, invalid trainer, missing
  // spell, unmet prereqs, and insufficient gold.
  EXPECT_EQ(ValidateRidingTrain(true, true, true, true, 40000, 999999),
            "you are in combat");
  EXPECT_EQ(ValidateRidingTrain(false, false, true, true, 40000, 999999),
            "cannot train riding here");
  EXPECT_EQ(ValidateRidingTrain(false, true, false, true, 40000, 999999),
            "riding skill not taught here");
  EXPECT_EQ(ValidateRidingTrain(false, true, true, false, 40000, 999999),
            "cannot learn that riding skill");
  EXPECT_EQ(ValidateRidingTrain(false, true, true, true, 40000, 39999),
            "not enough money");
}

TEST(RidingMountsTest, SelectMountPurchasesDeduplicatesTiersAndChecksUpgrades) {
  const std::vector<MountVendorCandidate> vendor_mounts = {
      // Three 60% ground color variants: cheapest/lowest ID wins.
      {5656, "Brown Horse Bridle", 9000, 60, false, false, false, true},
      {5655, "Chestnut Mare Bridle", 9000, 60, false, false, false, true},
      {2411, "Black Stallion Bridle", 9500, 60, false, false, false, true},
      // 100% ground upgrade (more expensive).
      {18777, "Swift Brown Steed", 100000, 100, false, false, false, true},
      // Flying mount.
      {25470, "Golden Gryphon", 500000, 150, true, false, false, true},
      // Unusable mount (wrong race/rep/skill).
      {8631, "Reins of the Striped Frostsaber", 9000, 60, false, false, false,
       false},
  };

  // Level 20 bot with 1g (10000c) and no mounts: gets one 60% ground mount.
  const std::vector<MountPurchaseOption> lvl20 =
      SelectMountPurchases(vendor_mounts, 10000, 0, 0, "Katie Hunter");
  ASSERT_EQ(lvl20.size(), 1u);
  EXPECT_EQ(lvl20[0].item_id, 5655u);
  EXPECT_EQ(lvl20[0].label,
            "Buy Chestnut Mare Bridle (60% mount, 90s) from Katie Hunter");

  // Counter-test: insufficient gold (1 copper short) -> nothing offered.
  EXPECT_TRUE(
      SelectMountPurchases(vendor_mounts, 8999, 0, 0, "Katie Hunter").empty());

  // Bot already owns a 60% ground mount and has 10g: only 100% upgrade offered.
  const std::vector<MountPurchaseOption> upgrade =
      SelectMountPurchases(vendor_mounts, 100000, 60, 0, "Katie Hunter");
  ASSERT_EQ(upgrade.size(), 1u);
  EXPECT_EQ(upgrade[0].item_id, 18777u);
  EXPECT_EQ(upgrade[0].label,
            "Buy Swift Brown Steed (100% mount, 10g) from Katie Hunter");

  // Bot already owns 100% ground mount and has 60g: only flying mount offered.
  const std::vector<MountPurchaseOption> flying =
      SelectMountPurchases(vendor_mounts, 600000, 100, 0, "Katie Hunter");
  ASSERT_EQ(flying.size(), 1u);
  EXPECT_EQ(flying[0].item_id, 25470u);
  EXPECT_EQ(flying[0].label,
            "Buy Golden Gryphon (150% flying mount, 50g) from Katie Hunter");

  // Counter-test: already known or already in bags is never re-offered.
  std::vector<MountVendorCandidate> owned = {
      {5655, "Chestnut Mare Bridle", 9000, 60, false, true, false, true},
      {5656, "Brown Horse Bridle", 9000, 60, false, false, true, true},
  };
  EXPECT_TRUE(
      SelectMountPurchases(owned, 100000, 0, 0, "Katie Hunter").empty());
}

TEST(RidingMountsTest, MountOfferAndValidationEnforceCombatAndIndoorChecks) {
  MountState ok;
  EXPECT_TRUE(CanOfferMount(ok));
  EXPECT_TRUE(ValidateMountState(ok, /*has_spell=*/true).empty());

  // Adversarial counter-tests: combat, indoors, dungeon, swimming, mounted,
  // flight path, vehicle, dead, unknown spell.
  {
    MountState s = ok;
    s.in_combat = true;
    EXPECT_FALSE(CanOfferMount(s));
    EXPECT_EQ(ValidateMountState(s, true), "cannot mount in combat");
  }
  {
    MountState s = ok;
    s.outdoors = false;
    EXPECT_FALSE(CanOfferMount(s));
    EXPECT_EQ(ValidateMountState(s, true), "cannot mount indoors");
  }
  {
    MountState s = ok;
    s.in_dungeon = true;
    EXPECT_FALSE(CanOfferMount(s));
    EXPECT_EQ(ValidateMountState(s, true), "cannot mount inside a dungeon");
  }
  {
    MountState s = ok;
    s.swimming = true;
    EXPECT_FALSE(CanOfferMount(s));
    EXPECT_EQ(ValidateMountState(s, true), "cannot mount while swimming");
  }
  {
    MountState s = ok;
    s.mounted = true;
    EXPECT_FALSE(CanOfferMount(s));
    EXPECT_EQ(ValidateMountState(s, true), "already mounted");
  }
  {
    MountState s = ok;
    s.in_flight = true;
    EXPECT_FALSE(CanOfferMount(s));
    EXPECT_EQ(ValidateMountState(s, true),
              "cannot mount while on a flight path");
  }
  {
    MountState s = ok;
    s.in_vehicle = true;
    EXPECT_FALSE(CanOfferMount(s));
    EXPECT_EQ(ValidateMountState(s, true), "cannot mount while in a vehicle");
  }
  {
    MountState s = ok;
    s.alive = false;
    EXPECT_FALSE(CanOfferMount(s));
    EXPECT_EQ(ValidateMountState(s, true), "you are dead");
  }
  EXPECT_EQ(ValidateMountState(ok, /*has_spell=*/false),
            "you don't know that mount");
}

TEST(RidingMountsTest, PickBestMountSelectsFlyingWhenCanFlyAndGroundOtherwise) {
  const std::vector<KnownMountSpell> mounts = {
      {472, "Pinto", 60, 0, false},
      {23229, "Swift Brown Steed", 100, 0, false},
      {32235, "Golden Gryphon", 60, 150, true},
      {32242, "Swift Blue Gryphon", 100, 280, true},
  };

  // Flyable zone (e.g. Northrend with Cold Weather Flying, Outland, Dalaran
  // Krasus' Landing): picks fastest flying mount (280% over 150% and ground).
  const std::optional<KnownMountSpell> fly =
      PickBestMount(mounts, /*can_fly=*/true);
  ASSERT_TRUE(fly.has_value());
  EXPECT_EQ(fly->spell_id, 32242u);

  // Counter-test: non-flyable zone (Azeroth, Dalaran streets, Northrend without
  // Cold Weather Flying, indoors): picks fastest ground mount (100%), NEVER a
  // flying mount.
  const std::optional<KnownMountSpell> ground =
      PickBestMount(mounts, /*can_fly=*/false);
  ASSERT_TRUE(ground.has_value());
  EXPECT_EQ(ground->spell_id, 23229u);

  // Counter-test: flyable zone when bot only knows ground mounts falls back to
  // fastest ground mount.
  const std::vector<KnownMountSpell> ground_only = {
      {472, "Pinto", 60, 0, false},
      {23229, "Swift Brown Steed", 100, 0, false},
  };
  const std::optional<KnownMountSpell> fallback =
      PickBestMount(ground_only, /*can_fly=*/true);
  ASSERT_TRUE(fallback.has_value());
  EXPECT_EQ(fallback->spell_id, 23229u);

  // Counter-test: flying-only mounts are NOT picked when `!can_fly`, ARE picked
  // when `can_fly`.
  const std::vector<KnownMountSpell> flying_only = {
      {32235, "Golden Gryphon", 60, 150, true},
      {32242, "Swift Blue Gryphon", 100, 280, true},
  };
  EXPECT_FALSE(PickBestMount(flying_only, /*can_fly=*/false).has_value());
  const std::optional<KnownMountSpell> fly_best =
      PickBestMount(flying_only, /*can_fly=*/true);
  ASSERT_TRUE(fly_best.has_value());
  EXPECT_EQ(fly_best->spell_id, 32242u);
}

TEST(RidingMountsTest, CanFlyInAreaChecksColdWeatherFlyingAndNoFlyZones) {
  // Outland outdoors (map 530): flyable without Cold Weather Flying.
  FlyableAreaInput outland;
  outland.map_id = kMapOutland;
  outland.area_is_flyable = true;
  EXPECT_TRUE(CanFlyInArea(outland));

  // Northrend outdoors / Dalaran Krasus' Landing (map 571, no-fly flag clear):
  // requires Cold Weather Flying.
  FlyableAreaInput northrend;
  northrend.map_id = kMapNorthrend;
  northrend.area_is_flyable = true;
  northrend.has_cold_weather_flying = true;
  EXPECT_TRUE(CanFlyInArea(northrend));

  // Adversarial counter-tests:
  // 1. Northrend WITHOUT Cold Weather Flying -> cannot fly.
  northrend.has_cold_weather_flying = false;
  EXPECT_FALSE(CanFlyInArea(northrend));

  // 2. Dalaran city streets (AREA_FLAG_NO_FLY_ZONE) -> cannot fly even with
  //    Cold Weather Flying.
  northrend.has_cold_weather_flying = true;
  northrend.area_no_fly_zone = true;
  EXPECT_FALSE(CanFlyInArea(northrend));

  // 3. Wintergrasp active battle -> cannot fly.
  northrend.area_no_fly_zone = false;
  northrend.battlefield_blocks_fly = true;
  EXPECT_FALSE(CanFlyInArea(northrend));

  // 4. Indoors or inside a dungeon -> cannot fly.
  northrend.battlefield_blocks_fly = false;
  northrend.outdoors = false;
  EXPECT_FALSE(CanFlyInArea(northrend));
  northrend.outdoors = true;
  northrend.in_dungeon = true;
  EXPECT_FALSE(CanFlyInArea(northrend));

  // 5. Eastern Kingdoms (map 0) / Kalimdor (map 1) -> cannot fly in WotLK even
  //    with Cold Weather Flying.
  FlyableAreaInput azeroth;
  azeroth.map_id = 0;
  azeroth.area_is_flyable = true;
  azeroth.has_cold_weather_flying = true;
  EXPECT_FALSE(CanFlyInArea(azeroth));
  azeroth.map_id = 1;
  EXPECT_FALSE(CanFlyInArea(azeroth));
}

TEST(RidingMountsTest, FlightMovementGatesAndStepAltitudeClearance) {
  // Free-flying outdoors out of combat -> flight movement allowed.
  EXPECT_TRUE(CanUseFlightMovement(/*can_fly_flag=*/true,
                                   /*has_fly_aura=*/true,
                                   /*in_flight_taxi=*/false,
                                   /*in_dungeon=*/false,
                                   /*in_combat=*/false));
  // Adversarial counter-tests: P0 combat priority, dungeon, taxi flight, or
  // ground-only mount must NOT use free-flight movement.
  EXPECT_FALSE(
      CanUseFlightMovement(true, true, false, false, /*in_combat=*/true));
  EXPECT_FALSE(
      CanUseFlightMovement(true, true, false, /*in_dungeon=*/true, false));
  EXPECT_FALSE(
      CanUseFlightMovement(true, true, /*in_flight_taxi=*/true, false, false));
  EXPECT_FALSE(CanUseFlightMovement(false, false, false, false, false));

  // Mid-flight step lifts above terrain/cliff clearance.
  const wlb::snapshot::Vec3 step = ComputeFlightStep(
      {0.0f, 0.0f, 100.0f}, {200.0f, 0.0f, 100.0f}, /*max_step_yd=*/70.0f,
      /*clearance_z_yd=*/12.0f, /*terrain_z=*/130.0f);
  EXPECT_NEAR(step.x, 70.0f, 0.01f);
  EXPECT_NEAR(step.y, 0.0f, 0.01f);
  EXPECT_NEAR(step.z, 142.0f, 0.01f);

  // Final approach within one step descends directly to destination.
  const wlb::snapshot::Vec3 land = ComputeFlightStep(
      {150.0f, 0.0f, 142.0f}, {200.0f, 0.0f, 100.0f}, /*max_step_yd=*/70.0f,
      /*clearance_z_yd=*/12.0f, /*terrain_z=*/98.0f);
  EXPECT_FLOAT_EQ(land.x, 200.0f);
  EXPECT_FLOAT_EQ(land.y, 0.0f);
  EXPECT_FLOAT_EQ(land.z, 100.0f);
}

TEST(RidingMountsTest, DismountOfferAndValidationEnforceFlightAndControl) {
  EXPECT_TRUE(CanOfferDismount(/*alive=*/true, /*mounted=*/true,
                               /*in_flight=*/false, /*incapacitated=*/false));
  EXPECT_TRUE(ValidateDismountState(/*mounted=*/true, /*in_flight=*/false,
                                    /*incapacitated=*/false)
                  .empty());

  // Counter-tests: dead, not mounted, on taxi flight, stunned/feared.
  EXPECT_FALSE(CanOfferDismount(false, true, false, false));
  EXPECT_FALSE(CanOfferDismount(true, false, false, false));
  EXPECT_FALSE(CanOfferDismount(true, true, true, false));
  EXPECT_FALSE(CanOfferDismount(true, true, false, true));

  EXPECT_EQ(ValidateDismountState(false, false, false), "not mounted");
  EXPECT_EQ(ValidateDismountState(true, true, false),
            "cannot dismount while on a flight path");
  EXPECT_EQ(ValidateDismountState(true, false, true),
            "cannot dismount right now");
}

}  // namespace
}  // namespace llm_brain
