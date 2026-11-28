// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/pull_discipline.h"

#include <limits>
#include <vector>

#include "base/time/time.h"
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
  // Eligibility policy (M1 P0-2): only grouped non-tanks in combat.
  EXPECT_TRUE(ShouldApplyStopDps(/*in_group=*/true, /*is_tank=*/false,
                                 /*bot_in_combat=*/true,
                                 /*party_in_combat=*/true));
  EXPECT_TRUE(ShouldApplyStopDps(/*in_group=*/true, /*is_tank=*/false,
                                 /*bot_in_combat=*/false,
                                 /*party_in_combat=*/true));
  EXPECT_TRUE(ShouldApplyStopDps(/*in_group=*/true, /*is_tank=*/false,
                                 /*bot_in_combat=*/true,
                                 /*party_in_combat=*/false));
  // Adversarial counter-tests: never apply to tanks, never out of combat,
  // never when solo.
  EXPECT_FALSE(ShouldApplyStopDps(/*in_group=*/true, /*is_tank=*/true,
                                  /*bot_in_combat=*/true,
                                  /*party_in_combat=*/true));
  EXPECT_FALSE(ShouldApplyStopDps(/*in_group=*/true, /*is_tank=*/false,
                                  /*bot_in_combat=*/false,
                                  /*party_in_combat=*/false));
  EXPECT_FALSE(ShouldApplyStopDps(/*in_group=*/false, /*is_tank=*/false,
                                  /*bot_in_combat=*/true,
                                  /*party_in_combat=*/true));

  StopDpsState state;
  // Out-of-combat RecordStopDps refuses to latch (never breaks the next fight).
  RecordStopDps(state, /*in_combat=*/false);
  EXPECT_FALSE(state.active);
  EXPECT_FALSE(state.saw_combat);

  // In-combat RecordStopDps activates and holds through combat.
  RecordStopDps(state, /*in_combat=*/true);
  EXPECT_TRUE(state.active);
  EXPECT_TRUE(state.saw_combat);
  EXPECT_FALSE(ShouldResetStopDps(state, /*bot_in_combat=*/true,
                                  /*party_in_combat=*/true));
  EXPECT_TRUE(state.active);

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

TEST(PullDisciplineTest, UnpulledHumanSkullGuardPreAndPostPull) {
  const UnpulledSkullGuardInput pre_pull{
      .in_group = true,
      .in_battleground = false,
      .has_skull_mark = true,
      .skull_in_combat_with_group = false,
      .explicit_target_override = false,
  };
  // Human marks an unpulled mob with Skull -> suppressed so bots don't pull.
  EXPECT_TRUE(ShouldSuppressUnpulledSkull(pre_pull));

  // Post-pull: once the skull mob is in combat with any group member, bots
  // immediately assist and focus Skull.
  UnpulledSkullGuardInput post_pull = pre_pull;
  post_pull.skull_in_combat_with_group = true;
  EXPECT_FALSE(ShouldSuppressUnpulledSkull(post_pull));

  // Adversarial counter-tests:
  // 1. Explicit leader "pull skull" / "kill skull" override is allowed.
  UnpulledSkullGuardInput explicit_pull = pre_pull;
  explicit_pull.explicit_target_override = true;
  EXPECT_FALSE(ShouldSuppressUnpulledSkull(explicit_pull));

  // 2. Solo bot is unaffected.
  UnpulledSkullGuardInput solo = pre_pull;
  solo.in_group = false;
  EXPECT_FALSE(ShouldSuppressUnpulledSkull(solo));

  // 3. Battleground is left to playerbots.
  UnpulledSkullGuardInput bg = pre_pull;
  bg.in_battleground = true;
  EXPECT_FALSE(ShouldSuppressUnpulledSkull(bg));

  // 4. No skull mark -> nothing to suppress.
  UnpulledSkullGuardInput no_skull = pre_pull;
  no_skull.has_skull_mark = false;
  EXPECT_FALSE(ShouldSuppressUnpulledSkull(no_skull));
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

TEST(PullDisciplineTest, HealerManaPullWaitHoldsUntilReadyWithHysteresis) {
  HealerManaWaitState state;
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(100);
  HealerManaMember healer{
      .is_healer = true,
      .online = true,
      .alive = true,
      .same_map = true,
      .uses_mana = true,
      .dist_yd = 15.0f,
      .mana_pct = 20.0f,
      .drinking = false,
  };

  // Below 35% mana: holds pulls out of combat.
  EXPECT_TRUE(EvaluateHealerManaPullWait(state, /*in_group=*/true,
                                         /*in_combat=*/false, {healer}, t0));
  EXPECT_TRUE(state.waiting);

  // Healer sits and drinks; climbs past 35% to 50% -> hysteresis keeps holding.
  healer.drinking = true;
  healer.mana_pct = 50.0f;
  EXPECT_TRUE(EvaluateHealerManaPullWait(state, /*in_group=*/true,
                                         /*in_combat=*/false, {healer},
                                         t0 + base::Seconds(5)));

  // Reaches 75% ready threshold -> releases pulls and clears state.
  healer.mana_pct = kHealerPullWaitReadyManaPct;
  EXPECT_FALSE(EvaluateHealerManaPullWait(state, /*in_group=*/true,
                                          /*in_combat=*/false, {healer},
                                          t0 + base::Seconds(15)));
  EXPECT_FALSE(state.waiting);
  EXPECT_FALSE(state.timed_out);

  // Healer drinking starting at 55% (above 35% start threshold) also holds
  // until 75%.
  healer.mana_pct = 55.0f;
  healer.drinking = true;
  EXPECT_TRUE(EvaluateHealerManaPullWait(state, /*in_group=*/true,
                                         /*in_combat=*/false, {healer},
                                         t0 + base::Seconds(20)));
  healer.mana_pct = 80.0f;
  healer.drinking = false;
  EXPECT_FALSE(EvaluateHealerManaPullWait(state, /*in_group=*/true,
                                          /*in_combat=*/false, {healer},
                                          t0 + base::Seconds(28)));
}

TEST(PullDisciplineTest, HealerManaPullWaitAdversarialAntiStallAndInvariants) {
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(1000);
  HealerManaMember healer{
      .is_healer = true,
      .online = true,
      .alive = true,
      .same_map = true,
      .uses_mana = true,
      .dist_yd = 20.0f,
      .mana_pct = 15.0f,
      .drinking = false,
  };

  // 1. Solo invariant: never holds pulls when not in a group, and clears state.
  {
    HealerManaWaitState state;
    EXPECT_TRUE(EvaluateHealerManaPullWait(state, true, false, {healer}, t0));
    EXPECT_FALSE(EvaluateHealerManaPullWait(state, /*in_group=*/false,
                                            /*in_combat=*/false, {healer},
                                            t0 + base::Seconds(1)));
    EXPECT_FALSE(state.waiting);
  }

  // 2. P0 combat priority: entering combat immediately releases & clears wait.
  {
    HealerManaWaitState state;
    EXPECT_TRUE(EvaluateHealerManaPullWait(state, true, false, {healer}, t0));
    EXPECT_FALSE(EvaluateHealerManaPullWait(state, /*in_group=*/true,
                                            /*in_combat=*/true, {healer},
                                            t0 + base::Seconds(1)));
    EXPECT_FALSE(state.waiting);
    EXPECT_FALSE(state.timed_out);
  }

  // 3. Anti-stall (no drink): OOM healer who never drinks (out of water or
  //    human who wants to pull) releases at kHealerNoDrinkTimeout and does NOT
  //    re-latch on subsequent ticks until mana recovers or combat resets it.
  {
    HealerManaWaitState state;
    EXPECT_TRUE(EvaluateHealerManaPullWait(state, true, false, {healer}, t0));
    EXPECT_TRUE(EvaluateHealerManaPullWait(
        state, true, false, {healer},
        t0 + kHealerNoDrinkTimeout - base::Seconds(1)));
    EXPECT_FALSE(EvaluateHealerManaPullWait(state, true, false, {healer},
                                            t0 + kHealerNoDrinkTimeout));
    EXPECT_TRUE(state.timed_out);
    // Subsequent tick while still OOM stays released (no oscillation).
    EXPECT_FALSE(EvaluateHealerManaPullWait(
        state, true, false, {healer},
        t0 + kHealerNoDrinkTimeout + base::Seconds(2)));
    // Once healer recovers to >= 75%, timed_out clears so future OOM waits
    // work.
    HealerManaMember recovered = healer;
    recovered.mana_pct = 80.0f;
    EXPECT_FALSE(EvaluateHealerManaPullWait(
        state, true, false, {recovered},
        t0 + kHealerNoDrinkTimeout + base::Seconds(5)));
    EXPECT_FALSE(state.timed_out);
    EXPECT_TRUE(EvaluateHealerManaPullWait(
        state, true, false, {healer},
        t0 + kHealerNoDrinkTimeout + base::Seconds(6)));
  }

  // 4. Anti-stall (max wait): healer stuck drinking forever releases at
  //    kHealerMaxWaitTimeout.
  {
    HealerManaWaitState state;
    HealerManaMember stuck = healer;
    stuck.drinking = true;
    stuck.mana_pct = 40.0f;
    EXPECT_TRUE(EvaluateHealerManaPullWait(state, true, false, {stuck}, t0));
    EXPECT_TRUE(EvaluateHealerManaPullWait(
        state, true, false, {stuck},
        t0 + kHealerMaxWaitTimeout - base::Seconds(1)));
    EXPECT_FALSE(EvaluateHealerManaPullWait(state, true, false, {stuck},
                                            t0 + kHealerMaxWaitTimeout));
    EXPECT_TRUE(state.timed_out);
  }

  // 5. Anti-stall (stopped drinking early): healer stops drinking at 50% mana
  //    -> releases kHealerNoDrinkTimeout after stopping.
  {
    HealerManaWaitState state;
    HealerManaMember drinking = healer;
    drinking.drinking = true;
    drinking.mana_pct = 20.0f;
    EXPECT_TRUE(EvaluateHealerManaPullWait(state, true, false, {drinking}, t0));
    drinking.drinking = false;
    drinking.mana_pct = 50.0f;
    EXPECT_TRUE(EvaluateHealerManaPullWait(state, true, false, {drinking},
                                           t0 + base::Seconds(4)));
    EXPECT_FALSE(EvaluateHealerManaPullWait(state, true, false, {drinking},
                                            t0 + kHealerNoDrinkTimeout));
    EXPECT_TRUE(state.timed_out);
  }

  // 6. Non-eligible members never stall pulls: dead, offline, other map,
  //    out of range, non-mana, NaN, or non-healer DPS.
  {
    HealerManaWaitState state;
    HealerManaMember dead = healer;
    dead.alive = false;
    HealerManaMember offline = healer;
    offline.online = false;
    HealerManaMember other_map = healer;
    other_map.same_map = false;
    HealerManaMember far_away = healer;
    far_away.dist_yd = kHealerManaWaitMaxDistYd + 5.0f;
    HealerManaMember no_mana = healer;
    no_mana.uses_mana = false;
    HealerManaMember nan_mana = healer;
    nan_mana.mana_pct = std::numeric_limits<float>::quiet_NaN();
    HealerManaMember oom_dps = healer;
    oom_dps.is_healer = false;
    EXPECT_FALSE(EvaluateHealerManaPullWait(
        state, true, false,
        {dead, offline, other_map, far_away, no_mana, nan_mana, oom_dps}, t0));
  }

  // 7. Logout / map-change / disconnect reset clears wait and timeout cleanly.
  {
    HealerManaWaitState state;
    EXPECT_FALSE(ClearHealerManaWait(state));
    EXPECT_TRUE(EvaluateHealerManaPullWait(state, true, false, {healer}, t0));
    EXPECT_TRUE(ClearHealerManaWait(state));
    EXPECT_FALSE(state.waiting);
    EXPECT_FALSE(state.timed_out);
    EXPECT_FALSE(ClearHealerManaWait(state));
  }
}

TEST(PullDisciplineTest, LeaderWaitExpiresOutOfCombatAndResetsOnInvariants) {
  const base::TimeTicks t0 = base::TimeTicks() + base::Seconds(500);

  // 1. Holds before timeout, expires at kLeaderWaitTimeout out of combat.
  {
    LeaderWaitState state;
    RecordLeaderWait(state, t0);
    EXPECT_TRUE(state.active);
    EXPECT_FALSE(ShouldExpireLeaderWait(
        state, /*in_group=*/true, /*policy_stay=*/true,
        /*bot_in_combat=*/false, /*party_in_combat=*/false,
        t0 + kLeaderWaitTimeout - base::Seconds(1)));
    EXPECT_TRUE(state.active);
    EXPECT_TRUE(ShouldExpireLeaderWait(
        state, /*in_group=*/true, /*policy_stay=*/true,
        /*bot_in_combat=*/false, /*party_in_combat=*/false,
        t0 + kLeaderWaitTimeout));
    EXPECT_FALSE(state.active);
    // Does not fire twice once cleared.
    EXPECT_FALSE(ShouldExpireLeaderWait(
        state, /*in_group=*/true, /*policy_stay=*/true,
        /*bot_in_combat=*/false, /*party_in_combat=*/false,
        t0 + kLeaderWaitTimeout + base::Minutes(1)));
  }

  // 2. Re-issuing "wait" refreshes the timeout window.
  {
    LeaderWaitState state;
    RecordLeaderWait(state, t0);
    RecordLeaderWait(state, t0 + base::Minutes(2));
    EXPECT_FALSE(ShouldExpireLeaderWait(
        state, /*in_group=*/true, /*policy_stay=*/true,
        /*bot_in_combat=*/false, /*party_in_combat=*/false,
        t0 + kLeaderWaitTimeout));
    EXPECT_TRUE(ShouldExpireLeaderWait(
        state, /*in_group=*/true, /*policy_stay=*/true,
        /*bot_in_combat=*/false, /*party_in_combat=*/false,
        t0 + base::Minutes(2) + kLeaderWaitTimeout));
  }

  // 3. P1 ("wait" in combat): an out-of-combat wait immediately releases when
  //    combat starts (bot or party), so healers/DPS move to the tank/target.
  {
    LeaderWaitState state;
    RecordLeaderWait(state, t0, /*issued_in_combat=*/false);
    EXPECT_TRUE(ShouldExpireLeaderWait(
        state, /*in_group=*/true, /*policy_stay=*/true,
        /*bot_in_combat=*/false, /*party_in_combat=*/true,
        t0 + base::Seconds(2)));
    EXPECT_FALSE(state.active);

    RecordLeaderWait(state, t0, /*issued_in_combat=*/false);
    EXPECT_TRUE(ShouldExpireLeaderWait(
        state, /*in_group=*/true, /*policy_stay=*/true,
        /*bot_in_combat=*/true, /*party_in_combat=*/false,
        t0 + base::Seconds(2)));
    EXPECT_FALSE(state.active);
  }

  // 3b. Adversarial counter-test: an explicit in-combat wait holds through
  //     that fight (never expires mid-combat), downgrades once combat ends so
  //     a later fight releases it, and expires after kLeaderWaitTimeout out of
  //     combat.
  {
    LeaderWaitState state;
    RecordLeaderWait(state, t0, /*issued_in_combat=*/true);
    EXPECT_FALSE(ShouldExpireLeaderWait(
        state, /*in_group=*/true, /*policy_stay=*/true,
        /*bot_in_combat=*/true, /*party_in_combat=*/false,
        t0 + kLeaderWaitTimeout + base::Minutes(1)));
    EXPECT_TRUE(state.active);
    EXPECT_FALSE(ShouldExpireLeaderWait(
        state, /*in_group=*/true, /*policy_stay=*/true,
        /*bot_in_combat=*/false, /*party_in_combat=*/true,
        t0 + kLeaderWaitTimeout + base::Minutes(1)));
    EXPECT_TRUE(state.active);
    // Combat ends before timeout -> downgrades issued_in_combat, so a new
    // pull releases it immediately.
    EXPECT_FALSE(ShouldExpireLeaderWait(
        state, /*in_group=*/true, /*policy_stay=*/true,
        /*bot_in_combat=*/false, /*party_in_combat=*/false,
        t0 + base::Seconds(10)));
    EXPECT_TRUE(state.active);
    EXPECT_FALSE(state.issued_in_combat);
    EXPECT_TRUE(ShouldExpireLeaderWait(
        state, /*in_group=*/true, /*policy_stay=*/true,
        /*bot_in_combat=*/false, /*party_in_combat=*/true,
        t0 + base::Seconds(15)));
    EXPECT_FALSE(state.active);
  }

  // 4. Solo / left group clears state without triggering follow mutation.
  {
    LeaderWaitState state;
    RecordLeaderWait(state, t0);
    EXPECT_FALSE(ShouldExpireLeaderWait(
        state, /*in_group=*/false, /*policy_stay=*/true,
        /*bot_in_combat=*/false, /*party_in_combat=*/false,
        t0 + kLeaderWaitTimeout));
    EXPECT_FALSE(state.active);
  }

  // 5. External stay=false (e.g. brain chose follow) clears state cleanly.
  {
    LeaderWaitState state;
    RecordLeaderWait(state, t0);
    EXPECT_FALSE(ShouldExpireLeaderWait(
        state, /*in_group=*/true, /*policy_stay=*/false,
        /*bot_in_combat=*/false, /*party_in_combat=*/false,
        t0 + kLeaderWaitTimeout));
    EXPECT_FALSE(state.active);
  }

  // 6. Logout / map-change / disconnect reset clears wait cleanly.
  {
    LeaderWaitState state;
    EXPECT_FALSE(ClearLeaderWait(state));
    RecordLeaderWait(state, t0);
    EXPECT_TRUE(ClearLeaderWait(state));
    EXPECT_FALSE(state.active);
    EXPECT_FALSE(ClearLeaderWait(state));
  }
}

TEST(PullDisciplineTest,
     BroadenedPulledDefinitionPetsTotemsEscortsFriendlyNpcs) {
  // Solo bot is never gated.
  EXPECT_TRUE(IsMobPulledForGroup({.in_group = false}));

  // Unrelated nearby idle pack in a group stays gated.
  EXPECT_FALSE(IsMobPulledForGroup({.in_group = true}));

  // Mob fighting a party member counts as pulled.
  EXPECT_TRUE(
      IsMobPulledForGroup({.in_group = true, .fights_party_member = true}));

  // Mob hitting a party member's pet or totem counts as pulled, even when
  // human_away_or_paused is set (bots must defend party pets/totems).
  EXPECT_TRUE(IsMobPulledForGroup({.in_group = true,
                                   .fights_party_pet_or_totem = true,
                                   .human_away_or_paused = true}));

  // Mob hitting an active escort NPC within kEscortProtectMaxDistYd counts as
  // pulled (even if human_away_or_paused is set).
  EXPECT_TRUE(IsMobPulledForGroup({
      .in_group = true,
      .fights_escort_npc = true,
      .escort_npc_dist_to_party_yd = kEscortProtectMaxDistYd,
      .human_away_or_paused = true,
  }));
  // Far-away escort NPC beyond range does not un-gate.
  EXPECT_FALSE(IsMobPulledForGroup({
      .in_group = true,
      .fights_escort_npc = true,
      .escort_npc_dist_to_party_yd = kEscortProtectMaxDistYd + 1.0f,
  }));

  // Mob attacking a friendly NPC near the party counts as pulled.
  EXPECT_TRUE(IsMobPulledForGroup({
      .in_group = true,
      .fights_friendly_npc = true,
      .friendly_npc_dist_to_party_yd = kFriendlyNpcProtectMaxDistYd,
  }));
  // Adversarial: distant unrelated friendly NPC skirmish stays gated.
  EXPECT_FALSE(IsMobPulledForGroup({
      .in_group = true,
      .fights_friendly_npc = true,
      .friendly_npc_dist_to_party_yd = kFriendlyNpcProtectMaxDistYd + 5.0f,
  }));
  // Adversarial: negative or NaN distance never un-gates.
  EXPECT_FALSE(IsMobPulledForGroup({
      .in_group = true,
      .fights_friendly_npc = true,
      .friendly_npc_dist_to_party_yd = -1.0f,
  }));
  EXPECT_FALSE(IsMobPulledForGroup({
      .in_group = true,
      .fights_escort_npc = true,
      .escort_npc_dist_to_party_yd = std::numeric_limits<float>::quiet_NaN(),
  }));
}

TEST(PullDisciplineTest, HumanQuestMobAssistanceCloseVsFarAndAdversarial) {
  // Open-world quest mob within kHumanQuestMobAssistMaxDistYd of the human is
  // engageable so bots can help the human finish quests faster than solo.
  EXPECT_TRUE(IsMobPulledForGroup({
      .in_group = true,
      .in_dungeon = false,
      .is_human_quest_mob = true,
      .human_quest_mob_dist_yd = kHumanQuestMobAssistMaxDistYd,
  }));
  EXPECT_TRUE(IsMobPulledForGroup({
      .in_group = true,
      .in_dungeon = false,
      .is_human_quest_mob = true,
      .human_quest_mob_dist_yd = 12.0f,
  }));

  // Far-away quest mob stays gated to prevent accidental pulling.
  EXPECT_FALSE(IsMobPulledForGroup({
      .in_group = true,
      .in_dungeon = false,
      .is_human_quest_mob = true,
      .human_quest_mob_dist_yd = kHumanQuestMobAssistMaxDistYd + 0.5f,
  }));

  // Adversarial: unrelated nearby mob (not a quest objective) stays gated.
  EXPECT_FALSE(IsMobPulledForGroup({
      .in_group = true,
      .in_dungeon = false,
      .is_human_quest_mob = false,
      .human_quest_mob_dist_yd = 5.0f,
  }));

  // Adversarial: inside a dungeon, quest mobs do NOT bypass the pull gate
  // (tank must still pull dungeon packs).
  EXPECT_FALSE(IsMobPulledForGroup({
      .in_group = true,
      .in_dungeon = true,
      .is_human_quest_mob = true,
      .human_quest_mob_dist_yd = 10.0f,
  }));

  // Adversarial: when human is AFK/offline or healer is OOM/drinking, unpulled
  // quest mobs stay gated.
  EXPECT_FALSE(IsMobPulledForGroup({
      .in_group = true,
      .in_dungeon = false,
      .is_human_quest_mob = true,
      .human_quest_mob_dist_yd = 10.0f,
      .human_away_or_paused = true,
  }));

  // Adversarial: NaN / negative distance never un-gates.
  EXPECT_FALSE(IsMobPulledForGroup({
      .in_group = true,
      .is_human_quest_mob = true,
      .human_quest_mob_dist_yd = -2.0f,
  }));
  EXPECT_FALSE(IsMobPulledForGroup({
      .in_group = true,
      .is_human_quest_mob = true,
      .human_quest_mob_dist_yd = std::numeric_limits<float>::quiet_NaN(),
  }));
}

TEST(PullDisciplineTest, PullGateHumanLedBotLedAndSoloInvariants) {
  // 1. Solo bot is never gated, regardless of other flags.
  EXPECT_FALSE(ShouldHoldPulls({
      .in_group = false,
      .offline_human_pause = true,
      .human_holds = true,
      .healer_mana_hold = true,
      .human_leads = true,
      .has_other_tank = true,
  }));

  // 2. Human-led group (non-tank human): tank bot may pull; DPS/healer holds.
  EXPECT_FALSE(ShouldHoldPulls({
      .in_group = true,
      .human_leads = true,
      .bot_tanks = true,
  }));
  EXPECT_TRUE(ShouldHoldPulls({
      .in_group = true,
      .human_leads = true,
      .bot_tanks = false,
  }));

  // 3. Human tank / AFK / behind, offline human, or healer mana wait holds
  //    even the bot tank.
  EXPECT_TRUE(ShouldHoldPulls({
      .in_group = true,
      .human_holds = true,
      .bot_tanks = true,
  }));
  EXPECT_TRUE(ShouldHoldPulls({
      .in_group = true,
      .offline_human_pause = true,
      .bot_tanks = true,
  }));
  EXPECT_TRUE(ShouldHoldPulls({
      .in_group = true,
      .healer_mana_hold = true,
      .bot_tanks = true,
  }));

  // 4. Bot-led group WITH a bot tank:
  //    - Tank bot (whether leader or not) may pull.
  EXPECT_FALSE(ShouldHoldPulls({
      .in_group = true,
      .bot_leads = true,
      .bot_tanks = true,
  }));
  EXPECT_FALSE(ShouldHoldPulls({
      .in_group = true,
      .bot_leads = false,
      .bot_tanks = true,
  }));
  //    - DPS/healer follower holds pulls (no `Attack (not pulled)`).
  EXPECT_TRUE(ShouldHoldPulls({
      .in_group = true,
      .bot_leads = false,
      .bot_tanks = false,
      .has_other_tank = true,
  }));
  //    - Non-tank bot leader also holds pulls when another tank is present.
  EXPECT_TRUE(ShouldHoldPulls({
      .in_group = true,
      .bot_leads = true,
      .bot_tanks = false,
      .has_other_tank = true,
  }));

  // 5. Bot-led group WITHOUT any tank:
  //    - Bot leader may pull so the group isn't deadlocked.
  EXPECT_FALSE(ShouldHoldPulls({
      .in_group = true,
      .bot_leads = true,
      .bot_tanks = false,
      .has_other_tank = false,
  }));
  //    - Bot follower still holds pulls and waits for the bot leader.
  EXPECT_TRUE(ShouldHoldPulls({
      .in_group = true,
      .bot_leads = false,
      .bot_tanks = false,
      .has_other_tank = false,
  }));
}

}  // namespace
}  // namespace llm_brain
