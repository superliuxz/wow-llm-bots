// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/raid_meta.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

constexpr size_t kMaxMissingBuffs = 4;
constexpr size_t kMaxDbmTimers = 6;
constexpr base::TimeDelta kMinNewCastGap = base::Milliseconds(1500);
constexpr base::TimeDelta kMinObservedInterval = base::Seconds(5);
constexpr base::TimeDelta kMaxObservedInterval = base::Seconds(180);

struct RaidBuffFamily {
  std::string_view group_spell;
  std::string_view single_spell;
  // Alternative aura names that satisfy the same buff family (e.g. scroll or
  // dalaran intellect).
  std::string_view alt_aura;
};

// Standard WotLK 3.3.5a raid/party buffs.
constexpr std::array<RaidBuffFamily, 9> kRaidBuffFamilies = {{
    {"Prayer of Fortitude", "Power Word: Fortitude", ""},
    {"Prayer of Spirit", "Divine Spirit", ""},
    {"Prayer of Shadow Protection", "Shadow Protection", ""},
    {"Arcane Brilliance", "Arcane Intellect", "Dalaran Brilliance"},
    {"Gift of the Wild", "Mark of the Wild", ""},
    {"Greater Blessing of Kings", "Blessing of Kings", ""},
    {"Greater Blessing of Might", "Blessing of Might", ""},
    {"Greater Blessing of Wisdom", "Blessing of Wisdom", ""},
    {"Greater Blessing of Sanctuary", "Blessing of Sanctuary", ""},
}};

bool ContainsIgnoreCase(const std::vector<std::string>& list,
                        std::string_view target) {
  if (target.empty()) {
    return false;
  }
  return std::any_of(list.begin(), list.end(), [&](const std::string& s) {
    return base::EqualsCaseInsensitiveASCII(s, target);
  });
}

std::string FormatLockoutRemaining(int64_t remaining_s) {
  if (remaining_s <= 0) {
    return "locked";
  }
  const int64_t days = remaining_s / 86400;
  const int64_t hours = (remaining_s % 86400) / 3600;
  const int64_t mins = (remaining_s % 3600) / 60;
  if (days > 0) {
    return base::StringPrintf("locked (%lldd %lldh)",
                              static_cast<long long>(days),
                              static_cast<long long>(hours));
  }
  if (hours > 0) {
    return base::StringPrintf("locked (%lldh %lldm)",
                              static_cast<long long>(hours),
                              static_cast<long long>(mins));
  }
  if (mins > 0) {
    return base::StringPrintf("locked (%lldm)", static_cast<long long>(mins));
  }
  return "locked (<1m)";
}

}  // namespace

std::optional<pb::Instance> BuildInstanceMeta(const InstanceMetaInput& in) {
  if (!in.is_dungeon_or_raid) {
    return std::nullopt;
  }
  pb::Instance inst;
  inst.name = in.map_name;
  if (!in.is_raid) {
    inst.size = in.max_players > 0 ? in.max_players : 5;
    inst.difficulty = (in.spawn_mode == kAcoreDungeonHeroic) ? "5H" : "5N";
  } else if (in.max_players >= 40) {
    inst.size = 40;
    inst.difficulty = "40N";
  } else {
    switch (in.spawn_mode) {
      case kAcoreRaid10Normal:
        inst.size = 10;
        inst.difficulty = "10N";
        break;
      case kAcoreRaid25Normal:
        inst.size = 25;
        inst.difficulty = "25N";
        break;
      case kAcoreRaid10Heroic:
        inst.size = 10;
        inst.difficulty = "10H";
        break;
      case kAcoreRaid25Heroic:
        inst.size = 25;
        inst.difficulty = "25H";
        break;
      default:
        inst.size = in.max_players > 0 ? in.max_players : 10;
        inst.difficulty = (inst.size >= 25) ? "25N" : "10N";
        break;
    }
  }
  if (in.perm_bound) {
    const int64_t remaining_s =
        (in.reset_time_epoch_s > 0 && in.now_epoch_s > 0)
            ? (in.reset_time_epoch_s - in.now_epoch_s)
            : 0;
    inst.locked_until = FormatLockoutRemaining(remaining_s);
  }
  return inst;
}

pb::DispelType MapAcoreDispelType(uint32_t acore_dispel) {
  switch (acore_dispel) {
    case kAcoreDispelMagic:
      return pb::DispelType::MAGIC;
    case kAcoreDispelCurse:
      return pb::DispelType::CURSE;
    case kAcoreDispelDisease:
      return pb::DispelType::DISEASE;
    case kAcoreDispelPoison:
      return pb::DispelType::POISON;
    default:
      return pb::DispelType::DISPEL_TYPE_UNSPECIFIED;
  }
}

std::vector<std::string> ComputeMissingRaidBuffs(
    const std::vector<std::string>& my_known_spells,
    const std::vector<std::vector<std::string>>& member_positive_auras) {
  std::vector<std::string> missing;
  if (my_known_spells.empty() || member_positive_auras.empty()) {
    return missing;
  }
  for (const RaidBuffFamily& fam : kRaidBuffFamilies) {
    if (missing.size() >= kMaxMissingBuffs) {
      break;
    }
    const bool knows_group =
        ContainsIgnoreCase(my_known_spells, fam.group_spell);
    const bool knows_single =
        ContainsIgnoreCase(my_known_spells, fam.single_spell);
    if (!knows_group && !knows_single) {
      continue;
    }
    const bool anyone_missing =
        std::any_of(member_positive_auras.begin(), member_positive_auras.end(),
                    [&](const std::vector<std::string>& auras) {
                      return !ContainsIgnoreCase(auras, fam.group_spell) &&
                             !ContainsIgnoreCase(auras, fam.single_spell) &&
                             !ContainsIgnoreCase(auras, fam.alt_aura);
                    });
    if (anyone_missing) {
      missing.emplace_back(knows_group ? fam.group_spell : fam.single_spell);
    }
  }
  return missing;
}

std::optional<pb::DbmWarningEvent> BuildDbmWarningFromBossChat(
    std::string_view sender,
    std::string_view text) {
  std::string cleaned;
  cleaned.reserve(text.size() + sender.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 1 < text.size() && text[i + 1] == 's') {
      if (!sender.empty()) {
        cleaned.append(sender);
      }
      ++i;
      continue;
    }
    cleaned.push_back(text[i]);
  }
  const std::string_view trimmed =
      base::TrimWhitespaceASCII(cleaned, base::TRIM_ALL);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  pb::DbmWarningEvent ev;
  ev.text = std::string(trimmed);
  return ev;
}

void BossTimerTracker::Reset() {
  boss_guid_ = 0;
  current_cast_.reset();
  history_.clear();
}

void BossTimerTracker::Update(uint64_t boss_guid,
                              bool boss_in_combat,
                              std::optional<ActiveCast> active_cast,
                              base::TimeTicks now) {
  if (boss_guid == 0 || !boss_in_combat) {
    Reset();
    return;
  }
  if (boss_guid != boss_guid_) {
    Reset();
    boss_guid_ = boss_guid;
  }
  if (!active_cast || active_cast->spell_name.empty() ||
      active_cast->remaining_s <= 0.0f) {
    if (current_cast_ && now >= current_cast_->expires_at) {
      current_cast_.reset();
    }
    return;
  }

  const base::TimeDelta rem = base::Seconds(active_cast->remaining_s);
  current_cast_ = ActiveTimer{active_cast->spell_name, now + rem};

  auto it =
      std::find_if(history_.begin(), history_.end(), [&](const CastHistory& h) {
        return base::EqualsCaseInsensitiveASCII(h.spell_name,
                                                active_cast->spell_name);
      });
  if (it == history_.end()) {
    history_.push_back(CastHistory{
        .spell_name = active_cast->spell_name,
        .last_seen_at = now,
        .observed_interval = std::nullopt,
    });
  } else if (now - it->last_seen_at >= kMinNewCastGap) {
    const base::TimeDelta gap = now - it->last_seen_at;
    if (gap >= kMinObservedInterval && gap <= kMaxObservedInterval) {
      it->observed_interval = gap;
    }
    it->last_seen_at = now;
  }
}

std::vector<pb::Encounter_Timer> BossTimerTracker::GetTimers(
    base::TimeTicks now) const {
  std::vector<pb::Encounter_Timer> timers;
  if (boss_guid_ == 0) {
    return timers;
  }
  if (current_cast_ && current_cast_->expires_at > now) {
    const float rem =
        static_cast<float>((current_cast_->expires_at - now).InSecondsF());
    if (rem > 0.1f) {
      timers.push_back(pb::Encounter_Timer{current_cast_->name, rem});
    }
  }
  for (const CastHistory& h : history_) {
    if (!h.observed_interval) {
      continue;
    }
    const base::TimeTicks next_at = h.last_seen_at + *h.observed_interval;
    if (next_at <= now) {
      continue;
    }
    const float rem = static_cast<float>((next_at - now).InSecondsF());
    if (rem > 0.1f) {
      timers.push_back(pb::Encounter_Timer{"Next " + h.spell_name, rem});
    }
  }
  std::sort(timers.begin(), timers.end(),
            [](const pb::Encounter_Timer& a, const pb::Encounter_Timer& b) {
              if (a.remaining_s != b.remaining_s) {
                return a.remaining_s < b.remaining_s;
              }
              return a.name < b.name;
            });
  if (timers.size() > kMaxDbmTimers) {
    timers.resize(kMaxDbmTimers);
  }
  return timers;
}

}  // namespace llm_brain
