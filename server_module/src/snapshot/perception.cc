// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/perception.h"
#include <math.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <tuple>
#include <vector>

#include "base/time/time.h"
#include "proto/snapshot.h"
#include "snapshot/invisible_displays.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

constexpr float kHeightSlackYd = 3;

// The client's grey-level rule (Acore::XP::GetGrayLevel).
int GrayLevel(int level) {
  if (level <= 5) {
    return 0;
  }
  if (level <= 39) {
    return level - 5 - level / 10;
  }
  if (level <= 59) {
    return level - 1 - level / 5;
  }
  return level - 9;
}

}  // namespace

pb::Direction CompassDirection(float dx, float dy, float here_yd) {
  if (std::hypot(dx, dy) < here_yd) {
    return pb::HERE;
  }
  // Bearing clockwise from north: east is -y.
  float degrees = std::atan2(-dy, dx) * 180.0f / static_cast<float>(M_PI);
  if (degrees < 0) {
    degrees += 360;
  }
  static constexpr pb::Direction kSectors[] = {pb::N, pb::NE, pb::E, pb::SE,
                                               pb::S, pb::SW, pb::W, pb::NW};
  const int sector = static_cast<int>(std::lround(degrees / 45.0f)) % 8;
  return kSectors[sector];
}

pb::Relative::Height HeightOf(float dz) {
  if (dz > kHeightSlackYd) {
    return pb::Relative::ABOVE;
  }
  if (dz < -kHeightSlackYd) {
    return pb::Relative::BELOW;
  }
  return pb::Relative::LEVEL;
}

pb::Unit::LevelColor LevelColor(int my_level, int their_level) {
  if (their_level >= my_level + 10) {
    return pb::Unit::SKULL_LEVEL;
  }
  if (their_level >= my_level + 5) {
    return pb::Unit::RED;
  }
  if (their_level >= my_level + 3) {
    return pb::Unit::ORANGE;
  }
  if (their_level >= my_level - 2) {
    return pb::Unit::YELLOW;
  }
  if (their_level > GrayLevel(my_level)) {
    return pb::Unit::GREEN;
  }
  return pb::Unit::GRAY;
}

const char* ClassName(uint8_t class_id) {
  static constexpr const char* kNames[] = {
      "",       "Warrior",      "Paladin", "Hunter", "Rogue",
      "Priest", "Death Knight", "Shaman",  "Mage",   "Warlock",
      "",       "Druid"};
  return class_id < std::size(kNames) ? kNames[class_id] : "";
}

pb::Unit::Size SizeFromBoundingRadius(float radius) {
  if (radius < 0.3f) {
    return pb::Unit::SMALL;
  }
  if (radius < 1.0f) {
    return pb::Unit::MEDIUM;
  }
  if (radius < 3.0f) {
    return pb::Unit::LARGE;
  }
  return pb::Unit::HUGE;
}

bool IsInvisibleModel(uint32_t display_id) {
  return std::binary_search(std::begin(kInvisibleDisplays),
                            std::end(kInvisibleDisplays), display_id);
}

bool HiddenFromPlayers(bool is_trigger, uint32_t display_id) {
  return is_trigger || IsInvisibleModel(display_id);
}

LosCache::LosCache(float near_yd,
                   base::TimeDelta near_ttl,
                   base::TimeDelta far_ttl)
    : near_yd_(near_yd), near_ttl_(near_ttl), far_ttl_(far_ttl) {}

LosCache::~LosCache() = default;

bool LosCache::IsFresh(const Entry& entry, base::TimeTicks now) const {
  const base::TimeDelta ttl = entry.dist_yd < near_yd_ ? near_ttl_ : far_ttl_;
  return !entry.stale && now - entry.checked < ttl;
}

std::optional<bool> LosCache::Get(uint64_t guid) const {
  const auto it = entries_.find(guid);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  return it->second.in_los;
}

void LosCache::Put(uint64_t guid,
                   bool in_los,
                   float dist_yd,
                   base::TimeTicks now) {
  entries_[guid] = Entry{in_los, dist_yd, now};
}

std::vector<uint64_t> LosCache::PickChecks(std::vector<Candidate> candidates,
                                           base::TimeTicks now,
                                           size_t budget) const {
  std::erase_if(candidates, [&](const Candidate& candidate) {
    const auto it = entries_.find(candidate.guid);
    return it != entries_.end() && IsFresh(it->second, now);
  });
  // Never checked: nearest first. Stale: oldest check first.
  const auto key = [&](const Candidate& candidate) {
    const auto it = entries_.find(candidate.guid);
    const bool known = it != entries_.end();
    return std::make_tuple(known,
                           known ? it->second.checked : base::TimeTicks(),
                           candidate.dist_yd);
  };
  std::sort(
      candidates.begin(), candidates.end(),
      [&](const Candidate& a, const Candidate& b) { return key(a) < key(b); });
  std::vector<uint64_t> picked;
  for (const Candidate& candidate : candidates) {
    if (picked.size() == budget) {
      break;
    }
    picked.push_back(candidate.guid);
  }
  return picked;
}

void LosCache::SetViewer(uint32_t map_id,
                         float x,
                         float y,
                         float z,
                         float moved_yd) {
  if (viewer_) {
    const float dx = x - viewer_->x, dy = y - viewer_->y, dz = z - viewer_->z;
    if (viewer_->map_id == map_id &&
        dx * dx + dy * dy + dz * dz <= moved_yd * moved_yd) {
      return;  // Still about where the results were taken.
    }
    if (viewer_->map_id != map_id) {
      entries_.clear();  // Another map: nothing carries over.
    } else {
      // Moved: every result is due again, but kept until re-checked, so a
      // running bot (7 yd/s) doesn't lose sight of everything past the ray
      // budget on every snapshot.
      for (auto& [guid, entry] : entries_) {
        entry.stale = true;
      }
    }
  }
  viewer_ = Viewer{map_id, x, y, z};
}

void LosCache::Prune(base::TimeTicks now, base::TimeDelta max_age) {
  std::erase_if(entries_, [&](const auto& entry) {
    return now - entry.second.checked > max_age;
  });
}

}  // namespace llm_brain
