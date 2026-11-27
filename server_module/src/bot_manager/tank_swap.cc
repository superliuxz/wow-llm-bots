// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/tank_swap.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "base/synchronization/lock.h"

namespace llm_brain {
namespace {

uint32_t ElapsedMs(uint32_t earlier_ms, uint32_t now_ms) {
  return static_cast<uint32_t>(now_ms - earlier_ms);
}

}  // namespace

bool IsReflexTauntActionName(std::string_view action_name) {
  return action_name == "taunt" || action_name == "taunt spell" ||
         action_name == "taunt on snare target" ||
         action_name == "heroic throw taunt" ||
         action_name == "battle shout taunt" || action_name == "mocking blow" ||
         action_name == "challenging shout" ||
         action_name == "hand of reckoning" ||
         action_name == "righteous defense" || action_name == "growl" ||
         action_name == "challenging roar" || action_name == "dark command";
}

bool ShouldSuppressReflexTaunt(const ReflexTauntCheck& c) {
  // Solo bots never have reflex taunts suppressed.
  if (!c.in_group || c.target_guid == 0) {
    return false;
  }
  // If this bot itself claimed the target via a brain taunt, let it hold
  // aggro unless another living tank currently holds or taunted the target.
  if (c.has_active_claim && c.claiming_tank_guid == c.bot_guid) {
    return c.target_taunted_by_other_tank || c.victim_is_other_living_tank;
  }
  // Another living group tank currently taunted or is tanking this target.
  if (c.target_taunted_by_other_tank || c.victim_is_other_living_tank) {
    return true;
  }
  // Another living group tank's brain just issued a taunt on this target:
  // yield during the grace window so the swap lands cleanly.
  if (c.has_active_claim && c.claiming_tank_guid != c.bot_guid &&
      c.claiming_tank_alive && c.claim_within_grace) {
    return true;
  }
  return false;
}

// static
TankSwapTracker& TankSwapTracker::Instance() {
  static auto* tracker = new TankSwapTracker();
  return *tracker;
}

void TankSwapTracker::RecordClaim(const TankSwapClaim& claim) {
  if (claim.target_guid == 0 || claim.claiming_tank_guid == 0 ||
      claim.group_guid == 0) {
    return;
  }
  base::AutoLock lock(lock_);
  // Prune stale claims while updating.
  std::erase_if(claims_, [&](const TankSwapClaim& existing) {
    return existing.target_guid == claim.target_guid ||
           ElapsedMs(existing.claimed_at_ms, claim.claimed_at_ms) >
               kMaxTankSwapClaimAgeMs;
  });
  claims_.push_back(claim);
}

std::optional<TankSwapClaim> TankSwapTracker::FindClaim(uint64_t target_guid,
                                                        uint64_t group_guid,
                                                        uint32_t map_id,
                                                        uint32_t instance_id,
                                                        uint32_t now_ms) const {
  if (target_guid == 0 || group_guid == 0) {
    return std::nullopt;
  }
  base::AutoLock lock(lock_);
  for (const TankSwapClaim& c : claims_) {
    if (c.target_guid == target_guid && c.group_guid == group_guid &&
        c.map_id == map_id && c.instance_id == instance_id &&
        ElapsedMs(c.claimed_at_ms, now_ms) <= kMaxTankSwapClaimAgeMs) {
      return c;
    }
  }
  return std::nullopt;
}

// static
bool TankSwapTracker::IsWithinGrace(const TankSwapClaim& claim,
                                    uint32_t now_ms) {
  return ElapsedMs(claim.claimed_at_ms, now_ms) <= kBrainTauntGraceMs;
}

void TankSwapTracker::ClearForBot(uint64_t bot_guid) {
  if (bot_guid == 0) {
    return;
  }
  base::AutoLock lock(lock_);
  std::erase_if(claims_, [bot_guid](const TankSwapClaim& c) {
    return c.claiming_tank_guid == bot_guid;
  });
}

void TankSwapTracker::ClearForTarget(uint64_t target_guid) {
  if (target_guid == 0) {
    return;
  }
  base::AutoLock lock(lock_);
  std::erase_if(claims_, [target_guid](const TankSwapClaim& c) {
    return c.target_guid == target_guid;
  });
}

void TankSwapTracker::ClearAll() {
  base::AutoLock lock(lock_);
  claims_.clear();
}

size_t TankSwapTracker::size() const {
  base::AutoLock lock(lock_);
  return claims_.size();
}

}  // namespace llm_brain
