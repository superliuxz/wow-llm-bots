// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/position_facts.h"

#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "proto/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

pb::OptionFacts::Spot Spot(const char* id, float x, float y) {
  pb::OptionFacts::Spot spot;
  spot.id = id;
  spot.pos = {x, y, 0};
  return spot;
}

pb::OptionFacts::Place PlaceAt(const char* id, float x, float y) {
  pb::OptionFacts::Place place;
  place.id = id;
  place.name = id;
  place.pos = {x, y, 0};
  return place;
}

pb::Objective Objective(uint32_t quest_id, bool complete, float dist_yd) {
  pb::Objective obj;
  obj.quest.emplace().quest_id = quest_id;
  obj.quest->quest = "Quest " + std::to_string(quest_id);
  obj.quest->complete = complete;
  obj.where.emplace().name = "Area " + std::to_string(quest_id);
  obj.where->rel.dist_yd = dist_yd;
  obj.where->rel.position = {dist_yd, 0, 0};
  return obj;
}

TEST(PositionFactsTest, RefreshMeasuresFromTheNewPositionAndKeepsIds) {
  PositionFacts facts;
  facts.spots = {Spot("spot:a", 100, 0), Spot("spot:b", 0, -300)};
  facts.places = {PlaceAt("place:town", -900, 0)};

  RefreshFrom({0, 0, 0}, &facts);
  ASSERT_EQ(facts.spots.size(), 2u);
  EXPECT_EQ(facts.spots[0].dir, pb::N);  // +x is north.
  EXPECT_FLOAT_EQ(facts.spots[0].dist_yd, 100);
  EXPECT_EQ(facts.spots[1].dir, pb::E);  // -y is east.
  EXPECT_EQ(facts.places[0].dir, pb::S);

  // Walking 90 yd north: spot a is now 10 yd away (too near to offer), the
  // others keep their ids with new distances.
  RefreshFrom({90, 0, 0}, &facts);
  ASSERT_EQ(facts.spots.size(), 1u);
  EXPECT_EQ(facts.spots[0].id, "spot:b");
  EXPECT_FLOAT_EQ(facts.places[0].dist_yd, 990);
}

TEST(PositionFactsTest, DropsWhatIsOutOfTravelRange) {
  PositionFacts facts;
  facts.places = {PlaceAt("place:near", 10, 0), PlaceAt("place:far", 3000, 0),
                  PlaceAt("place:ok", 500, 0)};
  RefreshFrom({0, 0, 0}, &facts);
  ASSERT_EQ(facts.places.size(), 1u);
  EXPECT_EQ(facts.places[0].id, "place:ok");
}

TEST(PositionFactsTest, MinimapShowsOnlyHubsInRangeMeasuredFromPos) {
  PositionFacts facts;
  pb::Place& near = facts.minimap_hubs.emplace_back();
  near.name = "Near";
  near.rel.position = {0, 120, 0};
  pb::Place& far = facts.minimap_hubs.emplace_back();
  far.name = "Far";
  far.rel.position = {190, 0, 0};

  pb::OptionFacts out;
  pb::Minimap minimap;
  AddPositionFacts(facts, {0, 0, 0}, {}, &out, &minimap);
  ASSERT_EQ(minimap.places.size(), 1u);
  EXPECT_EQ(minimap.places[0].name, "Near");
  EXPECT_FLOAT_EQ(minimap.places[0].rel.dist_yd, 120);
  EXPECT_EQ(minimap.places[0].rel.dir, pb::W);  // +y is west.
}

TEST(PositionFactsTest, NearestTurnInAndNearestObjectiveBecomePlaces) {
  const std::vector<pb::Objective> tracker = {
      Objective(1, /*complete=*/true, 400), Objective(2, true, 200),
      Objective(3, false, 900), Objective(4, false, 300),
      Objective(5, false, 10)};  // Too near to walk to.
  pb::OptionFacts out;
  pb::Minimap minimap;
  AddPositionFacts({}, {0, 0, 0}, tracker, &out, &minimap);
  ASSERT_EQ(out.places.size(), 2u);
  EXPECT_EQ(out.places[0].id, "place:quest:2:turnin");
  EXPECT_EQ(out.places[0].services, "turn in: Quest 2");
  EXPECT_EQ(out.places[1].id, "place:quest:4:objective");
  EXPECT_EQ(out.places[1].services, "quest: Quest 4");
  // None is within minimap range to draw (the 10 yd one is skipped).
  EXPECT_TRUE(minimap.objective_areas.empty());
}

}  // namespace
}  // namespace llm_brain
