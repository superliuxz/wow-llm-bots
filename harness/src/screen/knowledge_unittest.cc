// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "screen/knowledge.h"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>

#include "gtest/gtest.h"
#include "snapshot/snapshot.h"

namespace harness {
namespace {

constexpr const char* kSampleKnowledgeJson = R"json({
  "version": 1,
  "quests": {
    "7": {
      "title": "Kobold Camp Cleanup",
      "level": 2,
      "min_level": 1,
      "log": "Kill 8 Kobold Vermin.",
      "starter": "Marshal McBride",
      "ender": "Marshal McBride",
      "tip": "Kobold Vermin are north of the abbey."
    },
    "60": {
      "title": "Kobold Candles",
      "level": 7,
      "min_level": 4,
      "log": "Collect 8 Large Candles.",
      "ender": "William Pestle",
      "drops": ["Kobold Miner", "Kobold Geomancer"]
    }
  },
  "zones": {
    "Elwynn Forest": {
      "min_level": 1,
      "max_level": 10,
      "faction": "Alliance",
      "next_zones": ["Westfall (10-20)", "Loch Modan (10-20)"]
    }
  },
  "dungeons": {
    "Deadmines": {
      "level": "15-21",
      "summary": "Pull Defias patrols carefully."
    }
  },
  "bosses": {
    "Loken": {
      "abilities": "Lightning Nova: 20 yd AoE, 5 sec cast.",
      "strategy": "Stay close for Pulsing Shockwave, run 20 yd away on Lightning Nova."
    },
    "Kael'thas Sunstrider": {
      "abilities": "Pyroblast: heavy fire nuke.",
      "strategy": "Tempest Keep five-phase raid encounter."
    },
    "Magisters' Terrace::Kael'thas Sunstrider": {
      "abilities": "Shock Barrier: shield before Pyroblast.",
      "strategy": "Magisters' Terrace 5-man: break Shock Barrier and interrupt Pyroblast."
    },
    "Murmur": {
      "abilities": "Sonic Boom: 35 yd AoE.",
      "strategy": "Run out for Sonic Boom."
    },
    "Thrall": {
      "abilities": "Chain Lightning.",
      "strategy": "Give the Horde as little warning as possible."
    }
  }
})json";

TEST(WorldKnowledgeTest, OutleveledZoneAndActiveQuests) {
  std::string err;
  const std::optional<WorldKnowledge> wk =
      WorldKnowledge::ParseJson(kSampleKnowledgeJson, &err);
  ASSERT_TRUE(wk.has_value()) << err;
  EXPECT_EQ(wk->quest_count(), 2u);
  EXPECT_EQ(wk->zone_count(), 1u);
  EXPECT_EQ(wk->dungeon_count(), 1u);

  wlb::snapshot::Snapshot snap;
  snap.base.self.unit.level = 14;
  snap.base.where.zone = "Elwynn Forest";
  snap.base.where.in_instance = false;

  wlb::snapshot::Objective q_complete;
  q_complete.quest.emplace().quest_id = 7;
  q_complete.quest->complete = true;
  snap.ext.quest_tracker.push_back(q_complete);

  wlb::snapshot::Objective q_open;
  q_open.quest.emplace().quest_id = 60;
  q_open.quest->complete = false;
  snap.ext.quest_tracker.push_back(q_open);

  const std::string md = wk->SelectRelevant(snap, /*has_boss_notes=*/false);
  EXPECT_NE(md.find("Zone: Elwynn Forest (levels 1-10), Alliance"),
            std::string::npos);
  EXPECT_NE(md.find("Outleveled zone: you are level 14; next zones: Westfall "
                    "(10-20), Loch Modan (10-20)"),
            std::string::npos);
  EXPECT_NE(md.find("Kobold Candles"), std::string::npos);
  EXPECT_NE(md.find("Drops from: Kobold Miner, Kobold Geomancer."),
            std::string::npos);
  EXPECT_NE(md.find("Kobold Camp Cleanup"), std::string::npos);
  EXPECT_NE(md.find("turn in to Marshal McBride."), std::string::npos);
}

TEST(WorldKnowledgeTest, DungeonFallbackSuppressedWhenBossNotesExist) {
  const std::optional<WorldKnowledge> wk =
      WorldKnowledge::ParseJson(kSampleKnowledgeJson);
  ASSERT_TRUE(wk.has_value());

  wlb::snapshot::Snapshot snap;
  snap.base.where.in_instance = true;
  snap.base.where.zone = "Deadmines";
  wlb::snapshot::Objective q_open;
  q_open.quest.emplace().quest_id = 60;
  q_open.quest->complete = false;
  snap.ext.quest_tracker.push_back(q_open);

  const std::string without_notes =
      wk->SelectRelevant(snap, /*has_boss_notes=*/false);
  EXPECT_NE(without_notes.find("Instance: Deadmines (levels 15-21)"),
            std::string::npos);
  EXPECT_NE(without_notes.find("Pull Defias patrols carefully."),
            std::string::npos);
  EXPECT_NE(without_notes.find("Kobold Candles"), std::string::npos);

  const std::string with_notes =
      wk->SelectRelevant(snap, /*has_boss_notes=*/true);
  EXPECT_TRUE(with_notes.empty());
}

TEST(WorldKnowledgeTest, BossStrategyLookupInstanceScopedAndTargetPriority) {
  const std::optional<WorldKnowledge> wk =
      WorldKnowledge::ParseJson(kSampleKnowledgeJson);
  ASSERT_TRUE(wk.has_value());
  EXPECT_EQ(wk->boss_count(), 5u);

  wlb::snapshot::Snapshot snap;
  snap.base.where.in_instance = true;
  snap.base.where.zone = "Grand Magister's Asylum";
  snap.base.where.map = "Magisters' Terrace";
  snap.base.self.unit.target = "0xkael";

  // Friendly own-faction leader and totems must be ignored even if in bosses_.
  wlb::snapshot::Unit friendly_thrall;
  friendly_thrall.guid = "0xthrall";
  friendly_thrall.name = "Thrall";
  friendly_thrall.kind = wlb::snapshot::Unit::NPC;
  friendly_thrall.reaction = wlb::snapshot::Unit::FRIENDLY;
  friendly_thrall.rel.dist_yd = 2.0f;
  snap.base.nearby.units.push_back(friendly_thrall);

  wlb::snapshot::Unit totem;
  totem.guid = "0xtotem";
  totem.name = "Loken";
  totem.kind = wlb::snapshot::Unit::TOTEM;
  totem.reaction = wlb::snapshot::Unit::HOSTILE;
  totem.rel.dist_yd = 3.0f;
  snap.base.nearby.units.push_back(totem);

  // Dead boss skipped.
  wlb::snapshot::Unit dead_murmur;
  dead_murmur.guid = "0xdead";
  dead_murmur.name = "Murmur";
  dead_murmur.kind = wlb::snapshot::Unit::NPC;
  dead_murmur.reaction = wlb::snapshot::Unit::HOSTILE;
  dead_murmur.dead = true;
  dead_murmur.rel.dist_yd = 4.0f;
  snap.base.nearby.units.push_back(dead_murmur);

  // Closer untargeted boss (Loken, 10 yd) and duplicate Loken (12 yd).
  wlb::snapshot::Unit loken1;
  loken1.guid = "0xloken1";
  loken1.name = "Loken";
  loken1.kind = wlb::snapshot::Unit::NPC;
  loken1.reaction = wlb::snapshot::Unit::HOSTILE;
  loken1.rel.dist_yd = 10.0f;
  snap.base.nearby.units.push_back(loken1);

  wlb::snapshot::Unit loken2 = loken1;
  loken2.guid = "0xloken2";
  loken2.rel.dist_yd = 12.0f;
  snap.base.nearby.units.push_back(loken2);

  // Farther living Murmur (NaN dist -> treated as +inf, capped out by 2-boss
  // max).
  wlb::snapshot::Unit murmur_far;
  murmur_far.guid = "0xmurmur";
  murmur_far.name = "Murmur";
  murmur_far.kind = wlb::snapshot::Unit::NPC;
  murmur_far.reaction = wlb::snapshot::Unit::HOSTILE;
  murmur_far.rel.dist_yd = std::numeric_limits<float>::quiet_NaN();
  snap.base.nearby.units.push_back(murmur_far);

  // Farther targeted boss (Kael'thas, 25 yd) must rank AHEAD of closer
  // untargeted Loken (10 yd), and match via `where.map` ("Magisters' Terrace")
  // when `where.zone` is a subzone.
  wlb::snapshot::Unit kael;
  kael.guid = "0xkael";
  kael.name = "Kael'thas Sunstrider";
  kael.kind = wlb::snapshot::Unit::NPC;
  kael.reaction = wlb::snapshot::Unit::HOSTILE;
  kael.rel.dist_yd = 25.0f;
  snap.base.nearby.units.push_back(kael);

  const std::string md = wk->SelectRelevant(snap, /*has_boss_notes=*/false);
  const size_t pos_kael =
      md.find("Boss Kael'thas Sunstrider — Abilities: Shock Barrier");
  const size_t pos_loken = md.find("Boss Loken — Abilities: Lightning Nova");
  ASSERT_NE(pos_kael, std::string::npos);
  ASSERT_NE(pos_loken, std::string::npos);
  // Targeted Kael'thas appears before closer untargeted Loken.
  EXPECT_LT(pos_kael, pos_loken);
  // Instance-scoped map fallback wins over unqualified Tempest Keep entry.
  EXPECT_NE(md.find("Magisters' Terrace 5-man"), std::string::npos);
  EXPECT_EQ(md.find("Tempest Keep five-phase"), std::string::npos);
  // Friendly Thrall excluded; Murmur capped out by kMaxBossesInPrompt=2; Loken
  // deduplicated.
  EXPECT_EQ(md.find("Thrall"), std::string::npos);
  EXPECT_EQ(md.find("Murmur"), std::string::npos);
  EXPECT_EQ(md.find("Boss Loken — Abilities:", pos_loken + 1),
            std::string::npos);

  // Active encounter boss works even when nearby.units is empty.
  wlb::snapshot::Snapshot enc_snap;
  enc_snap.base.where.in_instance = true;
  enc_snap.ext.encounter.emplace().boss.emplace().name = "Murmur";
  const std::string enc_md =
      wk->SelectRelevant(enc_snap, /*has_boss_notes=*/false);
  EXPECT_NE(enc_md.find("Boss Murmur — Strategy: Run out for Sonic Boom."),
            std::string::npos);

  // Suppressed when curated boss notes are present.
  EXPECT_TRUE(wk->SelectRelevant(snap, /*has_boss_notes=*/true).empty());
}

TEST(WorldKnowledgeTest, RejectsInvalidJson) {
  std::string err;
  EXPECT_FALSE(WorldKnowledge::ParseJson("{not json", &err).has_value());
  EXPECT_FALSE(err.empty());
}

TEST(WorldKnowledgeTest, PerBossWikiFallbackWhenCuratedNotesMissActiveBoss) {
  const std::optional<WorldKnowledge> wk =
      WorldKnowledge::ParseJson(kSampleKnowledgeJson);
  ASSERT_TRUE(wk.has_value());

  wlb::snapshot::Snapshot snap;
  snap.base.where.in_instance = true;
  snap.base.where.zone = "Deadmines";
  wlb::snapshot::Objective q_open;
  q_open.quest.emplace().quest_id = 60;
  q_open.quest->complete = false;
  snap.ext.quest_tracker.push_back(q_open);

  wlb::snapshot::Unit loken;
  loken.guid = "0xloken";
  loken.name = "Loken";
  loken.kind = wlb::snapshot::Unit::NPC;
  loken.reaction = wlb::snapshot::Unit::HOSTILE;
  loken.rel.dist_yd = 8.0f;
  snap.base.nearby.units.push_back(loken);

  wlb::snapshot::Unit murmur;
  murmur.guid = "0xmurmur";
  murmur.name = "Murmur";
  murmur.kind = wlb::snapshot::Unit::NPC;
  murmur.reaction = wlb::snapshot::Unit::HOSTILE;
  murmur.rel.dist_yd = 15.0f;
  snap.base.nearby.units.push_back(murmur);

  // Curated notes cover Loken only: Loken is suppressed, Murmur gets wiki
  // fallback, and generic dungeon overview / quest lore stay suppressed.
  const std::string loken_only =
      "# Halls of Lightning (HoL)\n\n## Loken\n- Run out on Lightning Nova.\n";
  const std::string fallback =
      wk->SelectRelevant(snap, /*has_boss_notes=*/true, loken_only);
  EXPECT_EQ(fallback.find("Boss Loken"), std::string::npos);
  EXPECT_NE(fallback.find("Boss Murmur — Abilities: Sonic Boom"),
            std::string::npos);
  EXPECT_EQ(fallback.find("Instance: Deadmines"), std::string::npos);
  EXPECT_EQ(fallback.find("Kobold Candles"), std::string::npos);

  // Compound heading covers both listed bosses -> no wiki fallback emitted.
  const std::string compound_notes =
      "# Raid\n\n## Council (Loken, Murmur)\n- Burn both.\n";
  EXPECT_TRUE(wk->SelectRelevant(snap, /*has_boss_notes=*/true, compound_notes)
                  .empty());

  // Title-only notes file (no `## ` sections) falls back for every nearby boss
  // without leaking instance overview or quests.
  const std::string title_only = "# Trial of the Champion (TOC)\n";
  const std::string title_fb =
      wk->SelectRelevant(snap, /*has_boss_notes=*/true, title_only);
  EXPECT_NE(title_fb.find("Boss Loken"), std::string::npos);
  EXPECT_NE(title_fb.find("Boss Murmur"), std::string::npos);
  EXPECT_EQ(title_fb.find("Instance: Deadmines"), std::string::npos);
  EXPECT_EQ(title_fb.find("Kobold Candles"), std::string::npos);

  // Adversarial counter-test: friendly NPCs, dead bosses, and totems must never
  // trigger per-boss wiki fallback even when absent from curated notes.
  wlb::snapshot::Snapshot adv;
  adv.base.where.in_instance = true;
  adv.base.where.zone = "Deadmines";
  wlb::snapshot::Unit friendly_thrall;
  friendly_thrall.guid = "0xthrall";
  friendly_thrall.name = "Thrall";
  friendly_thrall.kind = wlb::snapshot::Unit::NPC;
  friendly_thrall.reaction = wlb::snapshot::Unit::FRIENDLY;
  adv.base.nearby.units.push_back(friendly_thrall);
  wlb::snapshot::Unit dead_murmur = murmur;
  dead_murmur.dead = true;
  adv.base.nearby.units.push_back(dead_murmur);
  wlb::snapshot::Unit totem_loken = loken;
  totem_loken.kind = wlb::snapshot::Unit::TOTEM;
  adv.base.nearby.units.push_back(totem_loken);
  EXPECT_TRUE(
      wk->SelectRelevant(adv, /*has_boss_notes=*/true, title_only).empty());
}

}  // namespace
}  // namespace harness
