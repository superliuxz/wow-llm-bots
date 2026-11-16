// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/time/time.h"
#include "proto/snapshot.pb.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

// The same rule as the screen the brain reads (harness/src/screen_renderer.cc):
// what a player can do with this unit by clicking it.
std::optional<OptionTarget::Kind> UnitKind(const pb::Unit& u) {
  if (u.name().empty() || u.guid().empty()) {
    return std::nullopt;  // Too far to see who it is: nothing to click.
  }
  if (u.kind() != pb::Unit::NPC && u.kind() != pb::Unit::CRITTER) {
    return std::nullopt;
  }
  if (u.dead()) {
    return u.lootable() ? std::optional(OptionTarget::Kind::kLoot)
                        : std::nullopt;
  }
  if (u.reaction() == pb::Unit::HOSTILE || u.reaction() == pb::Unit::NEUTRAL) {
    return OptionTarget::Kind::kAttack;
  }
  return std::nullopt;
}

std::string OptionText(const OptionTarget& target) {
  switch (target.kind) {
    case OptionTarget::Kind::kAttack:
      return "Attack " + target.name;
    case OptionTarget::Kind::kLoot:
      return "Loot " + target.name;
    case OptionTarget::Kind::kKeep:
      return "Keep doing what I'm doing";
  }
  return "?";
}

}  // namespace

std::string_view OptionKindName(OptionTarget::Kind kind) {
  switch (kind) {
    case OptionTarget::Kind::kAttack:
      return "attack";
    case OptionTarget::Kind::kLoot:
      return "loot";
    case OptionTarget::Kind::kKeep:
      return "keep";
  }
  return "?";
}

std::vector<OptionTarget> AddOptions(pb::Snapshot* snapshot) {
  // One option per (kind, name), in the order the names first appear; its
  // target is the nearest unit of that name.
  std::vector<OptionTarget> targets;
  std::map<std::pair<OptionTarget::Kind, std::string>, size_t> index;
  std::vector<float> nearest;
  for (const pb::Unit& u : snapshot->base().nearby().units()) {
    const std::optional<OptionTarget::Kind> kind = UnitKind(u);
    if (!kind) {
      continue;
    }
    const std::string name(u.name());
    const auto [it, added] = index.try_emplace({*kind, name}, targets.size());
    if (added) {
      targets.push_back({*kind, std::string(u.guid()), name});
      nearest.push_back(u.rel().dist_yd());
    } else if (u.rel().dist_yd() < nearest[it->second]) {
      targets[it->second].guid = std::string(u.guid());
      nearest[it->second] = u.rel().dist_yd();
    }
  }
  targets.push_back({OptionTarget::Kind::kKeep, "", ""});
  for (const OptionTarget& target : targets) {
    pb::Option* option = snapshot->add_options();
    option->set_text(OptionText(target));
    option->set_guid(target.guid);
  }
  return targets;
}

OptionHistory::OptionHistory(base::TimeDelta window) : window_(window) {}

OptionHistory::~OptionHistory() = default;

void OptionHistory::Add(uint32_t seq,
                        base::TimeTicks built,
                        std::vector<OptionTarget> options) {
  entries_.push_back({seq, built, std::move(options)});
  while (entries_.front().built < built - window_) {
    entries_.pop_front();
  }
}

OptionHistory::Lookup OptionHistory::Find(uint32_t seq, uint32_t option) const {
  Lookup result;
  for (const Entry& entry : entries_) {
    if (entry.seq != seq) {
      continue;
    }
    if (option == 0 || option > entry.options.size()) {
      result.error = "no option " + std::to_string(option) + " in snapshot " +
                     std::to_string(seq);
      return result;
    }
    result.target = &entry.options[option - 1];
    result.snapshot_built = entry.built;
    return result;
  }
  result.error = "snapshot " + std::to_string(seq) + " is too old or unknown";
  return result;
}

}  // namespace llm_brain
