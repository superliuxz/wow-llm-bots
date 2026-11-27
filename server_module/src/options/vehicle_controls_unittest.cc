// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/vehicle_controls.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

pb::Snapshot MakeVehicleSnapshot(bool in_combat) {
  pb::Snapshot snap;
  snap.base.self.unit.name = "Aldric";
  snap.base.self.state = pb::Self::ALIVE;
  snap.base.self.in_combat = in_combat;
  pb::Unit& veh = snap.ext.vehicle.emplace();
  veh.guid = "0xF130006E00000001";
  veh.name = "Wyrmrest Defender";
  veh.kind = pb::Unit::VEHICLE;
  veh.health_pct = 88.0f;
  veh.vehicle = BuildVehiclePart(
      {{1, "", true}, {0, "Aldric", true}},
      {
          {50240, "Flame Spike", 0.0f, 0.0f, true, false, false},
          {50248, "Engulf in Flames", 10.0f, 4.5f, false, false, false},
          {50253, "Revivify", 0.0f, 0.0f, true, false, false},
          {50299, "Passive Scale", 0.0f, 0.0f, true, false, true},
      });
  return snap;
}

TEST(VehicleControlsTest, BuildVehiclePartSortsSeatsFiltersAndCapsAbilities) {
  std::vector<VehicleSpellInput> spells = {
      {0, "ZeroId", 0.0f, 0.0f, true, false, false},
      {100, "", 0.0f, 0.0f, true, false, false},
      {101, "PassiveAura", 0.0f, 0.0f, true, false, true},
      {102, "Flame Breath", 6.0f, 0.0f, true, false, false},
      {102, "Flame Breath Dup", 6.0f, 0.0f, true, false, false},
      {103, "Fireball", 12.0f, 3.0f, true, true, false},
      {104, "Spell 3", 0.0f, 0.0f, true, false, false},
      {105, "Spell 4", 0.0f, 0.0f, true, false, false},
      {106, "Spell 5", 0.0f, 0.0f, true, false, false},
      {107, "Spell 6", 0.0f, 0.0f, true, false, false},
      {108, "Spell 7 OverCap", 0.0f, 0.0f, true, false, false},
  };
  pb::VehiclePart part =
      BuildVehiclePart({{2, "Vesper", true}, {0, "Aldric", true}}, spells);

  ASSERT_EQ(part.seats.size(), 2u);
  EXPECT_EQ(part.seats[0].index, 0u);
  EXPECT_EQ(part.seats[0].occupant, "Aldric");
  EXPECT_EQ(part.seats[1].index, 2u);
  EXPECT_EQ(part.seats[1].occupant, "Vesper");

  ASSERT_EQ(part.abilities.size(), kMaxVehicleAbilities);
  EXPECT_EQ(part.abilities[0].id, 102u);
  EXPECT_EQ(part.abilities[0].name, "Flame Breath");
  ASSERT_TRUE(part.abilities[0].ability.has_value());
  EXPECT_TRUE(part.abilities[0].ability->usable);

  EXPECT_EQ(part.abilities[1].id, 103u);
  ASSERT_TRUE(part.abilities[1].ability.has_value());
  EXPECT_FALSE(part.abilities[1].ability->usable);  // on cooldown
  EXPECT_TRUE(part.abilities[1].ability->ground_targeted);
  EXPECT_FLOAT_EQ(part.abilities[1].ability->cooldown_remaining_s, 3.0f);
  EXPECT_EQ(part.abilities[5].id, 107u);
}

TEST(VehicleControlsTest, CollectVehicleSpellOptionsWorksInAndOutOfCombat) {
  // P0 combat priority: vehicle abilities must remain available during combat
  // (vehicle quests and boss vehicle phases happen in combat).
  for (bool in_combat : {false, true}) {
    const pb::Snapshot snap = MakeVehicleSnapshot(in_combat);
    const std::vector<VehicleSpellOption> opts = CollectVehicleSpellOptions(
        snap, /*can_cast_from_vehicle=*/true, /*incapacitated=*/false);
    ASSERT_EQ(opts.size(), 2u);
    EXPECT_EQ(opts[0].spell_id, 50240u);
    EXPECT_EQ(opts[0].label, "Use Flame Spike (vehicle)");
    EXPECT_EQ(opts[1].spell_id, 50253u);
    EXPECT_EQ(opts[1].label, "Use Revivify (vehicle)");
  }
}

TEST(VehicleControlsTest, ExitOfferAndValidationEnforceVehicleSeatAndControl) {
  EXPECT_TRUE(CanOfferVehicleExit(/*alive=*/true, /*in_vehicle=*/true,
                                  /*can_exit_seat=*/true,
                                  /*incapacitated=*/false));
  EXPECT_EQ(ValidateVehicleExitState(/*in_vehicle=*/true,
                                     /*can_exit_seat=*/true,
                                     /*incapacitated=*/false),
            "");

  // Adversarial: not in vehicle, dead, non-exitable seat, incapacitated.
  EXPECT_FALSE(CanOfferVehicleExit(false, true, true, false));
  EXPECT_FALSE(CanOfferVehicleExit(true, false, true, false));
  EXPECT_FALSE(CanOfferVehicleExit(true, true, false, false));
  EXPECT_FALSE(CanOfferVehicleExit(true, true, true, true));

  EXPECT_EQ(ValidateVehicleExitState(false, true, false), "not in a vehicle");
  EXPECT_EQ(ValidateVehicleExitState(true, true, true),
            "cannot leave vehicle right now");
  EXPECT_EQ(ValidateVehicleExitState(true, false, false),
            "cannot exit this vehicle seat");
}

TEST(VehicleControlsTest, AdversarialNotInVehicleAndExitOrDisconnectReset) {
  // 1. Not in vehicle: no spells offered and validators reject cleanly.
  pb::Snapshot snap = MakeVehicleSnapshot(/*in_combat=*/false);
  snap.ext.vehicle.reset();
  EXPECT_TRUE(CollectVehicleSpellOptions(snap, true, false).empty());
  EXPECT_EQ(
      ValidateVehicleSpellState(/*in_vehicle=*/false, true, false, true, false),
      "not in a vehicle");
  EXPECT_EQ(ValidateVehicleExitState(/*in_vehicle=*/false, true, false),
            "not in a vehicle");

  // 2. Vehicle exit / disconnect / map-change reset: once `ext.vehicle` is
  // cleared on the next snapshot, no stale vehicle abilities linger.
  pb::Snapshot active = MakeVehicleSnapshot(/*in_combat=*/true);
  ASSERT_EQ(CollectVehicleSpellOptions(active, true, false).size(), 2u);
  active.ext.vehicle.reset();
  EXPECT_TRUE(CollectVehicleSpellOptions(active, true, false).empty());
}

TEST(VehicleControlsTest, AdversarialDestroyedVehicleDeadOrIncapacitated) {
  pb::Snapshot snap = MakeVehicleSnapshot(/*in_combat=*/true);

  // Passenger seat without cast/control permission cannot offer vehicle spells.
  EXPECT_TRUE(CollectVehicleSpellOptions(snap, /*can_cast_from_vehicle=*/false,
                                         /*incapacitated=*/false)
                  .empty());
  // Incapacitated bot cannot cast vehicle spells.
  EXPECT_TRUE(CollectVehicleSpellOptions(snap, /*can_cast_from_vehicle=*/true,
                                         /*incapacitated=*/true)
                  .empty());
  // Destroyed vehicle cannot cast spells.
  snap.ext.vehicle->dead = true;
  EXPECT_TRUE(CollectVehicleSpellOptions(snap, true, false).empty());

  // Dead bot cannot cast vehicle spells.
  snap.ext.vehicle->dead = false;
  snap.base.self.state = pb::Self::DEAD_BODY;
  EXPECT_TRUE(CollectVehicleSpellOptions(snap, true, false).empty());

  // Validator counter-checks.
  EXPECT_EQ(ValidateVehicleSpellState(true, false, false, true, false),
            "vehicle is destroyed");
  EXPECT_EQ(ValidateVehicleSpellState(true, true, true, true, false),
            "cannot use vehicle ability right now");
  EXPECT_EQ(ValidateVehicleSpellState(true, true, false, false, false),
            "vehicle does not have that ability");
  EXPECT_EQ(ValidateVehicleSpellState(true, true, false, true, true),
            "vehicle ability is on cooldown");
  EXPECT_EQ(ValidateVehicleSpellState(true, true, false, true, false), "");
}

}  // namespace
}  // namespace llm_brain
