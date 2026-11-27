// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Long-range mmap walking (Explore / GoTo).

#ifndef MOD_LLM_BRAIN_OPTIONS_EXEC_MOVEMENT_H_
#define MOD_LLM_BRAIN_OPTIONS_EXEC_MOVEMENT_H_

#include <cstdint>
#include <string>

#include "base/time/time.h"
#include "options/action_types.h"
#include "options/leader_travel.h"
#include "options/position_facts.h"
#include "snapshot/snapshot.h"

class Player;

namespace llm_brain {

// State for an active multi-step mmap walk (Explore / Go to).
struct PendingWalk {
  uint32_t map_id = 0;
  wlb::snapshot::Vec3 dest;
  wlb::snapshot::Vec3 step_dest;
  bool has_step = false;
  WalkProgressTracker progress;
  base::TimeTicks last_path_at;
  uint8_t consecutive_seam_hops = 0;
  LeaderTravelArrival arrival;
  bool waiting_for_transport = false;
  bool boarded_transport = false;
  base::TimeTicks transport_wait_started;
};

struct WalkStepResult {
  bool arrived = false;
  std::string error;
  base::TimeDelta path_time;
};

// Advances an active Explore / GoTo walk in ~70 yd steps along the mmap path.
WalkStepResult TickLongWalk(Player* bot,
                            base::TimeTicks now,
                            PendingWalk* walk,
                            const DungeonNavConfig& dungeon_nav = {});

// Starts an Explore / GoTo walk.
WalkStepResult StartWalkAction(Player* bot,
                               const WalkAction& action,
                               PendingWalk* walk,
                               const DungeonNavConfig& dungeon_nav = {});

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_EXEC_MOVEMENT_H_
