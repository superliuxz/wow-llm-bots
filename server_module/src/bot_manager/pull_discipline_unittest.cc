// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/pull_discipline.h"

#include "gtest/gtest.h"

namespace llm_brain {
namespace {

TEST(PullDisciplineTest, RtiOverrideResetsAfterCombatEnds) {
  RtiOverrideState state;
  RecordRtiOverride(state, /*target_guid=*/0x1234, /*in_combat=*/false);
  EXPECT_TRUE(state.active);
  EXPECT_FALSE(state.saw_combat);

  // While approaching an unengaged living target before combat starts, the
  // leader's pull/kill mark must remain active.
  EXPECT_FALSE(
      ShouldResetRtiOverride(state, {.bot_in_combat = false,
                                     .party_in_combat = false,
                                     .target_alive_and_in_world = true}));
  EXPECT_TRUE(state.active);

  // Once combat starts, keep the override active throughout the fight.
  EXPECT_FALSE(
      ShouldResetRtiOverride(state, {.bot_in_combat = true,
                                     .party_in_combat = true,
                                     .target_alive_and_in_world = true}));
  EXPECT_TRUE(state.active);
  EXPECT_TRUE(state.saw_combat);

  // Adversarial counter-test: active combat rti still works during the fight
  // even if the bot briefly drops combat while the party is still fighting.
  EXPECT_FALSE(
      ShouldResetRtiOverride(state, {.bot_in_combat = false,
                                     .party_in_combat = true,
                                     .target_alive_and_in_world = false}));
  EXPECT_TRUE(state.active);

  // Once both bot and party drop combat, rti resets cleanly.
  EXPECT_TRUE(
      ShouldResetRtiOverride(state, {.bot_in_combat = false,
                                     .party_in_combat = false,
                                     .target_alive_and_in_world = true}));
  EXPECT_FALSE(state.active);
  EXPECT_EQ(state.target_guid, 0u);
  EXPECT_FALSE(state.saw_combat);

  // Subsequent ticks with no active override are no-ops.
  EXPECT_FALSE(
      ShouldResetRtiOverride(state, {.bot_in_combat = false,
                                     .party_in_combat = false,
                                     .target_alive_and_in_world = false}));
}

TEST(PullDisciplineTest, RtiOverrideSetMidCombatWorksAndResetsAfter) {
  RtiOverrideState state;
  RecordRtiOverride(state, /*target_guid=*/0x5678, /*in_combat=*/true);
  EXPECT_TRUE(state.active);
  EXPECT_TRUE(state.saw_combat);

  // Active combat rti stays set while fighting.
  EXPECT_FALSE(
      ShouldResetRtiOverride(state, {.bot_in_combat = true,
                                     .party_in_combat = true,
                                     .target_alive_and_in_world = true}));
  EXPECT_TRUE(state.active);

  // Combat ends -> resets immediately.
  EXPECT_TRUE(
      ShouldResetRtiOverride(state, {.bot_in_combat = false,
                                     .party_in_combat = false,
                                     .target_alive_and_in_world = false}));
  EXPECT_FALSE(state.active);
}

TEST(PullDisciplineTest, RtiOverrideResetsIfMarkedTargetDiesBeforeCombat) {
  RtiOverrideState state;
  RecordRtiOverride(state, /*target_guid=*/0x9999, /*in_combat=*/false);

  // Marked target died or despawned before the bot ever entered combat.
  EXPECT_TRUE(
      ShouldResetRtiOverride(state, {.bot_in_combat = false,
                                     .party_in_combat = false,
                                     .target_alive_and_in_world = false}));
  EXPECT_FALSE(state.active);
}

TEST(PullDisciplineTest, RtiOverrideClearsOnMapChangeOrDisconnect) {
  RtiOverrideState state;
  EXPECT_FALSE(ClearRtiOverride(state));

  RecordRtiOverride(state, /*target_guid=*/0x4242, /*in_combat=*/true);
  EXPECT_TRUE(ClearRtiOverride(state));
  EXPECT_FALSE(state.active);
  EXPECT_EQ(state.target_guid, 0u);
  EXPECT_FALSE(state.saw_combat);
  EXPECT_FALSE(ClearRtiOverride(state));
}

TEST(PullDisciplineTest, PetTauntSuppressionPolicy) {
  // Group with a non-pet tank disables pet taunt autocast.
  EXPECT_TRUE(ShouldSuppressPetTaunt(/*in_group=*/true,
                                     /*has_non_pet_tank=*/true));

  // Adversarial counter-tests:
  // Solo pet still taunts (even if has_non_pet_tank were somehow true).
  EXPECT_FALSE(ShouldSuppressPetTaunt(/*in_group=*/false,
                                      /*has_non_pet_tank=*/false));
  EXPECT_FALSE(ShouldSuppressPetTaunt(/*in_group=*/false,
                                      /*has_non_pet_tank=*/true));
  // Group without a non-pet tank (e.g. hunter + mage + priest) still lets the
  // pet taunt.
  EXPECT_FALSE(ShouldSuppressPetTaunt(/*in_group=*/true,
                                      /*has_non_pet_tank=*/false));
}

TEST(PullDisciplineTest, PetTauntSpellClassification) {
  // Growl (Hunter pet): Rank 1 and Rank 9.
  EXPECT_TRUE(IsPetTauntSpell(2649));
  EXPECT_TRUE(IsPetTauntSpell(14916));
  EXPECT_TRUE(IsPetTauntSpell(27047));
  EXPECT_TRUE(IsPetTauntSpell(61676));
  // Torment (Voidwalker): Rank 1 and Rank 8.
  EXPECT_TRUE(IsPetTauntSpell(3716));
  EXPECT_TRUE(IsPetTauntSpell(27270));
  EXPECT_TRUE(IsPetTauntSpell(47984));
  // Suffering (Voidwalker): Rank 1 and Rank 8.
  EXPECT_TRUE(IsPetTauntSpell(17735));
  EXPECT_TRUE(IsPetTauntSpell(33701));
  EXPECT_TRUE(IsPetTauntSpell(47990));
  // Anguish (Felguard): Rank 1 and Rank 4.
  EXPECT_TRUE(IsPetTauntSpell(33698));
  EXPECT_TRUE(IsPetTauntSpell(47993));

  // Adversarial counter-tests: non-taunt pet spells and player spells must not
  // be classified as pet taunts.
  EXPECT_FALSE(IsPetTauntSpell(0));
  EXPECT_FALSE(IsPetTauntSpell(17253));  // Bite (Rank 1)
  EXPECT_FALSE(IsPetTauntSpell(16827));  // Claw (Rank 1)
  EXPECT_FALSE(IsPetTauntSpell(1742));   // Cower
  EXPECT_FALSE(IsPetTauntSpell(355));    // Warrior Taunt
  EXPECT_FALSE(IsPetTauntSpell(6795));   // Druid Growl (bear form, not pet)
}

TEST(PullDisciplineTest, StopDpsLifecycleAndActionFilter) {
  StopDpsState state;
  RecordStopDps(state, /*in_combat=*/false);
  EXPECT_TRUE(state.active);
  EXPECT_FALSE(state.saw_combat);

  // Pre-combat hold stays active before combat begins.
  EXPECT_FALSE(ShouldResetStopDps(state, /*bot_in_combat=*/false,
                                  /*party_in_combat=*/false));
  EXPECT_TRUE(state.active);

  // Combat begins: stays active through the fight.
  EXPECT_FALSE(ShouldResetStopDps(state, /*bot_in_combat=*/true,
                                  /*party_in_combat=*/true));
  EXPECT_TRUE(state.active);
  EXPECT_TRUE(state.saw_combat);

  // Counter-test: bot briefly drops combat while raid is still fighting.
  EXPECT_FALSE(ShouldResetStopDps(state, /*bot_in_combat=*/false,
                                  /*party_in_combat=*/true));
  EXPECT_TRUE(state.active);

  // Once both bot and party drop combat, stop dps resets cleanly.
  EXPECT_TRUE(ShouldResetStopDps(state, /*bot_in_combat=*/false,
                                 /*party_in_combat=*/false));
  EXPECT_FALSE(state.active);
  EXPECT_FALSE(state.saw_combat);

  // Explicit clear (resume dps / attack command / map change / disconnect).
  RecordStopDps(state, /*in_combat=*/true);
  EXPECT_TRUE(ClearStopDps(state));
  EXPECT_FALSE(state.active);
  EXPECT_FALSE(ClearStopDps(state));

  // Action filter blocks offensive/attack actions without blocking heals or
  // movement.
  EXPECT_TRUE(IsStopDpsBlockedActionName("attack"));
  EXPECT_TRUE(IsStopDpsBlockedActionName("attack my target"));
  EXPECT_TRUE(IsStopDpsBlockedActionName("attack rti target"));
  EXPECT_TRUE(IsStopDpsBlockedActionName("melee"));
  EXPECT_TRUE(IsStopDpsBlockedActionName("shoot"));
  EXPECT_TRUE(IsStopDpsBlockedActionName("auto shot"));
  EXPECT_TRUE(IsStopDpsBlockedActionName("wand"));
  EXPECT_TRUE(IsStopDpsBlockedActionName("pet attack"));
  EXPECT_FALSE(IsStopDpsBlockedActionName("heal"));
  EXPECT_FALSE(IsStopDpsBlockedActionName("follow"));
  EXPECT_FALSE(IsStopDpsBlockedActionName("stay"));
  EXPECT_FALSE(IsStopDpsBlockedActionName("dispel"));
  EXPECT_FALSE(IsStopDpsBlockedActionName("resurrect"));
}

TEST(PullDisciplineTest, SkullMarkTargetRequiresActiveCombatAndNoCc) {
  const SkullMarkCandidate valid{
      .alive = true,
      .is_npc = true,
      .hostile = true,
      .has_guid = true,
      .dist_yd = 12.0f,
      .in_combat_with_group = true,
      .is_crowd_controlled = false,
      .has_raid_marker = false,
  };
  EXPECT_TRUE(IsEligibleSkullMarkTarget(valid));

  // Unpulled mob (not in active combat with the group) must never be offered
  // for Skull, because playerbots' AttackersValue treats Skull as an attacker
  // and chain-pulls its pack.
  SkullMarkCandidate unpulled = valid;
  unpulled.in_combat_with_group = false;
  EXPECT_FALSE(IsEligibleSkullMarkTarget(unpulled));

  // Crowd-controlled mob (Polymorph/Sap/Banish/etc.) must not be marked Skull.
  SkullMarkCandidate cc_mob = valid;
  cc_mob.is_crowd_controlled = true;
  EXPECT_FALSE(IsEligibleSkullMarkTarget(cc_mob));

  // Mob already holding a raid marker (e.g. Moon CC mark or Cross) must not be
  // overwritten with Skull.
  SkullMarkCandidate marked = valid;
  marked.has_raid_marker = true;
  EXPECT_FALSE(IsEligibleSkullMarkTarget(marked));

  // Boundary and sanity counter-tests: dead, non-NPC, non-hostile, missing
  // GUID, negative distance, and beyond 40 yd are rejected; exactly 40 yd is
  // allowed.
  SkullMarkCandidate at_max_range = valid;
  at_max_range.dist_yd = kSkullMarkMaxDistYd;
  EXPECT_TRUE(IsEligibleSkullMarkTarget(at_max_range));

  SkullMarkCandidate too_far = valid;
  too_far.dist_yd = kSkullMarkMaxDistYd + 0.1f;
  EXPECT_FALSE(IsEligibleSkullMarkTarget(too_far));

  SkullMarkCandidate dead = valid;
  dead.alive = false;
  EXPECT_FALSE(IsEligibleSkullMarkTarget(dead));

  SkullMarkCandidate player = valid;
  player.is_npc = false;
  EXPECT_FALSE(IsEligibleSkullMarkTarget(player));

  SkullMarkCandidate neutral = valid;
  neutral.hostile = false;
  EXPECT_FALSE(IsEligibleSkullMarkTarget(neutral));

  SkullMarkCandidate no_guid = valid;
  no_guid.has_guid = false;
  EXPECT_FALSE(IsEligibleSkullMarkTarget(no_guid));
}

TEST(PullDisciplineTest, WarlockCcStrategyGatingAndFearActionGuard) {
  // Grouped or dungeon Warlocks suppress playerbots' `cc` strategy (which casts
  // Fear on the moon CC target into adjacent packs).
  EXPECT_TRUE(ShouldSuppressWarlockCcStrategy(
      /*is_warlock=*/true, /*in_group=*/true, /*in_dungeon=*/false,
      /*in_battleground=*/false));
  EXPECT_TRUE(ShouldSuppressWarlockCcStrategy(
      /*is_warlock=*/true, /*in_group=*/false, /*in_dungeon=*/true,
      /*in_battleground=*/false));
  EXPECT_TRUE(ShouldSuppressWarlockCcStrategy(
      /*is_warlock=*/true, /*in_group=*/true, /*in_dungeon=*/true,
      /*in_battleground=*/false));
  EXPECT_EQ(WarlockCcStrategyDelta(/*is_warlock=*/true, /*in_group=*/true,
                                   /*in_dungeon=*/true,
                                   /*in_battleground=*/false),
            ",-cc");
  EXPECT_EQ(WarlockCcStrategyDelta(/*is_warlock=*/true, /*in_group=*/true,
                                   /*in_dungeon=*/false,
                                   /*in_battleground=*/false),
            ",-cc");

  // Adversarial counter-tests:
  // 1. Solo open-world Warlock keeps and restores `+cc`.
  EXPECT_FALSE(ShouldSuppressWarlockCcStrategy(
      /*is_warlock=*/true, /*in_group=*/false, /*in_dungeon=*/false,
      /*in_battleground=*/false));
  EXPECT_EQ(WarlockCcStrategyDelta(/*is_warlock=*/true, /*in_group=*/false,
                                   /*in_dungeon=*/false,
                                   /*in_battleground=*/false),
            ",+cc");

  // 2. Battleground/arena Warlock is unaffected (PvP Fear stays available).
  EXPECT_FALSE(ShouldSuppressWarlockCcStrategy(
      /*is_warlock=*/true, /*in_group=*/true, /*in_dungeon=*/false,
      /*in_battleground=*/true));
  EXPECT_TRUE(WarlockCcStrategyDelta(/*is_warlock=*/true, /*in_group=*/true,
                                     /*in_dungeon=*/false,
                                     /*in_battleground=*/true)
                  .empty());

  // 3. Non-Warlock classes (Mage Polymorph, Priest Shackle, Druid Hibernate,
  //    Shaman Hex, Hunter Freezing Trap) are never stripped of `cc`.
  EXPECT_FALSE(ShouldSuppressWarlockCcStrategy(
      /*is_warlock=*/false, /*in_group=*/true, /*in_dungeon=*/true,
      /*in_battleground=*/false));
  EXPECT_TRUE(WarlockCcStrategyDelta(/*is_warlock=*/false, /*in_group=*/true,
                                     /*in_dungeon=*/true,
                                     /*in_battleground=*/false)
                  .empty());

  // Belt-and-suspenders action guard blocks only CastCrowdControlSpellAction
  // for "fear" when grouped or in a dungeon outside battlegrounds.
  EXPECT_TRUE(ShouldBlockFearCcAction(/*is_cc_spell_action=*/true, "fear",
                                      /*in_group=*/true, /*in_dungeon=*/true,
                                      /*in_battleground=*/false));
  EXPECT_TRUE(ShouldBlockFearCcAction(/*is_cc_spell_action=*/true, "fear",
                                      /*in_group=*/true, /*in_dungeon=*/false,
                                      /*in_battleground=*/false));
  EXPECT_TRUE(ShouldBlockFearCcAction(/*is_cc_spell_action=*/true, "fear",
                                      /*in_group=*/false, /*in_dungeon=*/true,
                                      /*in_battleground=*/false));

  // Counter-tests: Banish, Polymorph, solo open-world Fear, BG Fear, and
  // non-CC actions are NOT blocked.
  EXPECT_FALSE(ShouldBlockFearCcAction(/*is_cc_spell_action=*/true, "banish",
                                       /*in_group=*/true, /*in_dungeon=*/true,
                                       /*in_battleground=*/false));
  EXPECT_FALSE(ShouldBlockFearCcAction(/*is_cc_spell_action=*/true, "polymorph",
                                       /*in_group=*/true, /*in_dungeon=*/true,
                                       /*in_battleground=*/false));
  EXPECT_FALSE(ShouldBlockFearCcAction(/*is_cc_spell_action=*/true, "fear",
                                       /*in_group=*/false, /*in_dungeon=*/false,
                                       /*in_battleground=*/false));
  EXPECT_FALSE(ShouldBlockFearCcAction(/*is_cc_spell_action=*/true, "fear",
                                       /*in_group=*/true, /*in_dungeon=*/false,
                                       /*in_battleground=*/true));
  EXPECT_FALSE(ShouldBlockFearCcAction(/*is_cc_spell_action=*/false, "fear",
                                       /*in_group=*/true, /*in_dungeon=*/true,
                                       /*in_battleground=*/false));
}

}  // namespace
}  // namespace llm_brain
