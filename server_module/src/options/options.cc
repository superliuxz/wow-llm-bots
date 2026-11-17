// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/options.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/time/time.h"
#include "proto/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

// The same rule as the screen the brain reads (harness/src/screen_renderer.cc):
// what a player can do with this unit by clicking it.
std::optional<OptionTarget::Kind> UnitKind(const pb::Unit& u) {
  if (u.name.empty() || u.guid.empty()) {
    return std::nullopt;  // Too far to see who it is: nothing to click.
  }
  if (u.kind != pb::Unit::NPC && u.kind != pb::Unit::CRITTER) {
    return std::nullopt;
  }
  if (u.dead) {
    return u.lootable ? std::optional(OptionTarget::Kind::kLoot) : std::nullopt;
  }
  if (u.reaction == pb::Unit::HOSTILE || u.reaction == pb::Unit::NEUTRAL) {
    return OptionTarget::Kind::kAttack;
  }
  return std::nullopt;
}

// What clicking this object does. Only kinds the executor can carry out:
// mailboxes, banks, barber chairs and the like open windows the bot can't use
// yet (no `interaction` extension), so they offer nothing.
std::optional<OptionTarget::Kind> ObjectKind(const pb::GameObject& go) {
  if (go.name.empty() || go.guid.empty() || !go.usable) {
    return std::nullopt;
  }
  switch (go.kind) {
    case pb::GameObject::HERB:
    case pb::GameObject::ORE:
      return OptionTarget::Kind::kGather;
    case pb::GameObject::CHEST:
    case pb::GameObject::QUEST_OBJECT:
    case pb::GameObject::DOOR:
    case pb::GameObject::PORTAL:
      return OptionTarget::Kind::kUse;
    default:
      return std::nullopt;
  }
}

// How a target is picked among things of the same name: one attacking the
// bot first (the one a player would hit back), then the nearest.
struct Rank {
  bool targeting_me = false;
  float dist_yd = 0;

  bool BetterThan(const Rank& other) const {
    if (targeting_me != other.targeting_me) {
      return targeting_me;
    }
    return dist_yd < other.dist_yd;
  }
};

// One option per (kind, name), in the order the names first appear; its
// target is the best-ranked thing of that name.
void AddTarget(OptionTarget::Kind kind,
               const std::string& guid,
               const std::string& name,
               Rank rank,
               std::vector<OptionTarget>* targets,
               std::vector<Rank>* ranks) {
  for (size_t i = 0; i < targets->size(); ++i) {
    if ((*targets)[i].kind == kind && (*targets)[i].name == name) {
      if (rank.BetterThan((*ranks)[i])) {
        (*targets)[i].guid = guid;
        (*ranks)[i] = rank;
      }
      return;
    }
  }
  targets->push_back({kind, guid, name});
  ranks->push_back(rank);
}

std::string OptionText(const OptionTarget& target) {
  switch (target.kind) {
    case OptionTarget::Kind::kAttack:
      return "Attack " + target.name;
    case OptionTarget::Kind::kLoot:
      return "Loot " + target.name;
    case OptionTarget::Kind::kGather:
      return "Gather " + target.name;
    case OptionTarget::Kind::kUse:
      return "Use " + target.name;
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
    case OptionTarget::Kind::kGather:
      return "gather";
    case OptionTarget::Kind::kUse:
      return "use";
    case OptionTarget::Kind::kKeep:
      return "keep";
  }
  return "?";
}

std::vector<OptionTarget> AddOptions(pb::Snapshot* snapshot) {
  std::vector<OptionTarget> targets;
  std::vector<Rank> ranks;
  // A dead player (body or ghost) can't attack, loot or click anything nearby.
  const bool alive = snapshot->base.self.state != pb::Self::DEAD_BODY &&
                     snapshot->base.self.state != pb::Self::GHOST;
  for (const pb::Unit& u : snapshot->base.nearby.units) {
    if (const std::optional<OptionTarget::Kind> kind = UnitKind(u);
        kind && alive) {
      AddTarget(*kind, u.guid, u.name, {u.targeting_me, u.rel.dist_yd},
                &targets, &ranks);
    }
  }
  for (const pb::GameObject& go : snapshot->base.nearby.objects) {
    if (const std::optional<OptionTarget::Kind> kind = ObjectKind(go);
        kind && alive) {
      AddTarget(*kind, go.guid, go.name, {false, go.rel.dist_yd}, &targets,
                &ranks);
    }
  }
  targets.push_back({OptionTarget::Kind::kKeep, "", ""});
  for (const OptionTarget& target : targets) {
    pb::Option& option = snapshot->options.emplace_back();
    option.text = OptionText(target);
    option.guid = target.guid;
  }
  return targets;
}

OptionHistory::OptionHistory(base::TimeDelta window) : window_(window) {}

OptionHistory::OptionHistory(OptionHistory&&) noexcept = default;

OptionHistory& OptionHistory::operator=(OptionHistory&&) noexcept = default;

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
