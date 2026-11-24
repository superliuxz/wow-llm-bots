// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_OPTIONS_OPTION_FACTS_H_
#define MOD_LLM_BRAIN_OPTIONS_OPTION_FACTS_H_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "options/position_facts.h"
#include "snapshot/snapshot.h"

class Player;
class WorldObject;

namespace llm_brain {

// Where the facts are seen from: what of the bot they depend on. Built from a
// live bot (ViewpointOf) or, for eval fixtures, from a saved snapshot
// (test/facts_dump.cc), so both run ComputePositionFacts.
struct Viewpoint {
  uint32_t map_id = 0;
  uint32_t zone_id = 0;
  uint32_t area_id = 0;
  uint32_t phase_mask = 1;  // PHASEMASK_NORMAL (Object.h).
  wlb::snapshot::Vec3 pos;
  uint8_t level = 1;
  uint8_t class_id = 0;
  bool alliance = false;  // Otherwise Horde.
};

Viewpoint ViewpointOf(Player* bot);

// The world's grind spots (labeled with what lives there) and friendly
// service hubs, indexed once from the creature spawns. Building it takes
// seconds (a zone lookup per spawn), so it is built at startup, before any bot
// logs in, never on a snapshot. World thread only.
class WorldPlaces {
 public:
  struct SpotBucket {
    uint32_t map_id = 0;
    uint32_t zone_id = 0;
    uint32_t min_level = 0;
    uint32_t max_level = 0;
    std::vector<std::string> mobs;
    wlb::snapshot::Vec3 centroid;
    uint32_t count = 0;

    // "boars, scorpids, level 1-3".
    std::string Label() const;
  };

  struct Hub {
    // "place:<map>:<x>:<y>": stable, and unique even when two hubs share an
    // area name.
    std::string id;
    uint32_t map_id = 0;
    std::string name;
    bool for_alliance = false;
    bool for_horde = false;
    bool has_innkeeper = false;
    bool has_vendor = false;
    bool has_repair = false;
    bool has_flight_master = false;
    bool has_quests = false;
    uint32_t min_quest_level = 0;
    uint32_t max_quest_level = 0;
    uint32_t class_trainer_mask = 0;
    wlb::snapshot::Vec3 pos;
    bool pos_from_innkeeper = false;
  };

  struct DungeonSpawn {
    uint32_t spawn_id = 0;
    std::string name;
    wlb::snapshot::Vec3 pos;
    uint32_t faction = 0;
    uint8_t spawn_mask = 0;
    bool is_boss = false;
  };

  static std::unique_ptr<WorldPlaces> Build();

  WorldPlaces(const WorldPlaces&) = delete;
  WorldPlaces& operator=(const WorldPlaces&) = delete;
  ~WorldPlaces();

  // The 50 yd bucket holding (x, y, z), or null.
  const SpotBucket* FindBucket(uint32_t map_id,
                               float x,
                               float y,
                               float z) const;
  const std::vector<DungeonSpawn>* DungeonSpawns(uint32_t map_id) const;
  const std::unordered_map<uint64_t, SpotBucket>& buckets() const {
    return buckets_;
  }
  const std::vector<Hub>& hubs() const { return hubs_; }

 private:
  WorldPlaces();

  std::unordered_map<uint64_t, SpotBucket> buckets_;
  std::vector<Hub> hubs_;
  std::unordered_map<uint32_t, std::vector<DungeonSpawn>> dungeon_spawns_;
};

// `path_owner` (the bot) enables the walkable-ray fallback when no grind spot
// is near; without one (an offline fixture dump) that fallback is skipped.
PositionFacts ComputePositionFacts(const WorldPlaces& world,
                                   const Viewpoint& view,
                                   const WorldObject* path_owner,
                                   const DungeonNavConfig& dungeon_nav = {});

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_OPTION_FACTS_H_
