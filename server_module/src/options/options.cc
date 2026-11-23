// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/options.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

bool HasActionableQuestMarker(pb::Unit::QuestMarker marker) {
  switch (marker) {
    case pb::Unit::QUEST_AVAILABLE:
    case pb::Unit::QUEST_AVAILABLE_LOW:
    case pb::Unit::QUEST_TURN_IN:
    case pb::Unit::QUEST_DAILY:
      return true;
    case pb::Unit::QUEST_MARKER_UNSPECIFIED:
    case pb::Unit::QUEST_INCOMPLETE:
      return false;
  }
  return false;
}

bool HasTalkNpcFlags(const OptionFacts& facts, std::string_view guid) {
  for (const OptionFacts::Npc& npc : facts.npcs) {
    if (npc.guid == guid) {
      return (npc.npc_flags & kTalkNpcFlags) != 0;
    }
  }
  return false;
}

bool IsMarkedNotAttackable(const OptionFacts& facts, std::string_view guid) {
  for (const OptionFacts::Npc& npc : facts.npcs) {
    if (npc.guid == guid) {
      return (npc.npc_flags & kNpcFlagNotAttackable) != 0;
    }
  }
  return false;
}

// What a player can do with this unit by clicking it.
std::optional<OptionTarget::Kind> UnitKind(const pb::Unit& u,
                                           bool in_combat,
                                           const OptionFacts& facts) {
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
    return IsMarkedNotAttackable(facts, u.guid)
               ? std::nullopt
               : std::optional(OptionTarget::Kind::kAttack);
  }
  if (u.reaction == pb::Unit::FRIENDLY && u.kind == pb::Unit::NPC &&
      !in_combat &&
      (HasActionableQuestMarker(u.quest_marker) ||
       HasTalkNpcFlags(facts, u.guid))) {
    return OptionTarget::Kind::kTalk;
  }
  return std::nullopt;
}

// What clicking this object does. Herbs and ore nodes require the bot's
// learned gathering skill to be at least `go.required_skill` (`0` for quest
// objects like Cactus Apple that use the herb animation without a skill).
std::optional<OptionTarget::Kind> ObjectKind(const pb::GameObject& go,
                                             const OptionFacts& facts) {
  if (go.name.empty() || go.guid.empty() || !go.usable) {
    return std::nullopt;
  }
  switch (go.kind) {
    case pb::GameObject::HERB:
      return go.required_skill <= facts.herbalism_skill
                 ? std::optional(OptionTarget::Kind::kGather)
                 : std::nullopt;
    case pb::GameObject::ORE:
      return go.required_skill <= facts.mining_skill
                 ? std::optional(OptionTarget::Kind::kGather)
                 : std::nullopt;
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

std::string RestText(const OptionFacts& facts) {
  if (facts.has_food && facts.has_drink) {
    return "Rest (eat and drink)";
  }
  if (facts.has_food) {
    return "Rest (eat)";
  }
  if (facts.has_drink) {
    return "Rest (drink)";
  }
  return "Rest (sit, no food)";
}

std::string_view DirText(pb::Direction dir) {
  if (dir == pb::DIRECTION_UNSPECIFIED || dir == pb::HERE) {
    return "";
  }
  return pb::EnumName(dir);
}

std::string ExploreText(const OptionFacts::Spot& spot) {
  const std::string_view dir = DirText(spot.dir);
  const int dist = static_cast<int>(std::lround(spot.dist_yd));
  std::string head =
      dir.empty()
          ? base::StringPrintf("Explore %d yd", dist)
          : base::StringPrintf("Explore %.*s, %d yd",
                               static_cast<int>(dir.size()), dir.data(), dist);
  if (!spot.label.empty()) {
    head += " (" + spot.label + ")";
  }
  return head;
}

std::string GoToText(const OptionFacts::Place& place) {
  const std::string_view dir = DirText(place.dir);
  const int dist = static_cast<int>(std::lround(place.dist_yd));
  std::string text = "Go to " + place.name;
  if (!place.services.empty()) {
    text += " (" + place.services + ")";
  }
  if (dist > 0) {
    text += base::StringPrintf(", %d yd", dist);
    if (!dir.empty()) {
      text.push_back(' ');
      text.append(dir);
    }
  }
  return text;
}

std::string TargetedOptionText(const OptionTarget& target) {
  switch (target.kind) {
    case OptionTarget::Kind::kAttack:
      return "Attack " + target.name;
    case OptionTarget::Kind::kLoot:
      return "Loot " + target.name;
    case OptionTarget::Kind::kGather:
      return "Gather " + target.name;
    case OptionTarget::Kind::kUse:
      return "Use " + target.name;
    case OptionTarget::Kind::kTalk:
      return "Talk to " + target.name;
    case OptionTarget::Kind::kRest:
    case OptionTarget::Kind::kExplore:
    case OptionTarget::Kind::kGoTo:
      return target.name;
    case OptionTarget::Kind::kKeep:
      return "Keep doing what I'm doing";
    case OptionTarget::Kind::kInvite:
      return "Invite " + target.name + " to group";
    case OptionTarget::Kind::kAcceptInvite:
      return "Accept group invite from " + target.name;
    case OptionTarget::Kind::kDeclineInvite:
      return "Decline group invite from " + target.name;
    case OptionTarget::Kind::kLeaveGroup:
      return "Leave group";
  }
  return "?";
}

pb::Option::Kind ToProtoKind(OptionTarget::Kind kind) {
  switch (kind) {
    case OptionTarget::Kind::kAttack:
      return pb::Option::ATTACK;
    case OptionTarget::Kind::kLoot:
      return pb::Option::LOOT;
    case OptionTarget::Kind::kGather:
      return pb::Option::GATHER;
    case OptionTarget::Kind::kUse:
      return pb::Option::USE;
    case OptionTarget::Kind::kTalk:
      return pb::Option::TALK;
    case OptionTarget::Kind::kRest:
      return pb::Option::REST;
    case OptionTarget::Kind::kExplore:
      return pb::Option::EXPLORE;
    case OptionTarget::Kind::kGoTo:
      return pb::Option::GO_TO;
    case OptionTarget::Kind::kKeep:
      return pb::Option::KEEP;
    case OptionTarget::Kind::kInvite:
      return pb::Option::INVITE;
    case OptionTarget::Kind::kAcceptInvite:
      return pb::Option::ACCEPT_INVITE;
    case OptionTarget::Kind::kDeclineInvite:
      return pb::Option::DECLINE_INVITE;
    case OptionTarget::Kind::kLeaveGroup:
      return pb::Option::LEAVE_GROUP;
  }
  return pb::Option::KIND_UNSPECIFIED;
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
    case OptionTarget::Kind::kTalk:
      return "talk";
    case OptionTarget::Kind::kRest:
      return "rest";
    case OptionTarget::Kind::kExplore:
      return "explore";
    case OptionTarget::Kind::kGoTo:
      return "go to";
    case OptionTarget::Kind::kKeep:
      return "keep";
    case OptionTarget::Kind::kInvite:
      return "invite";
    case OptionTarget::Kind::kAcceptInvite:
      return "accept invite";
    case OptionTarget::Kind::kDeclineInvite:
      return "decline invite";
    case OptionTarget::Kind::kLeaveGroup:
      return "leave group";
  }
  return "?";
}

std::vector<OptionTarget> AddOptions(pb::Snapshot* snapshot,
                                     const OptionFacts& facts) {
  std::vector<OptionTarget> targets;
  std::vector<Rank> ranks;
  const pb::Self& self = snapshot->base.self;
  // A dead player (body or ghost) can't attack, loot or click anything nearby.
  const bool alive =
      self.state != pb::Self::DEAD_BODY && self.state != pb::Self::GHOST;
  for (const pb::Unit& u : snapshot->base.nearby.units) {
    if (const std::optional<OptionTarget::Kind> kind =
            UnitKind(u, self.in_combat, facts);
        kind && alive) {
      AddTarget(*kind, u.guid, u.name, {u.targeting_me, u.rel.dist_yd},
                &targets, &ranks);
    }
  }
  for (const pb::GameObject& go : snapshot->base.nearby.objects) {
    if (const std::optional<OptionTarget::Kind> kind = ObjectKind(go, facts);
        kind && alive) {
      AddTarget(*kind, go.guid, go.name, {false, go.rel.dist_yd}, &targets,
                &ranks);
    }
  }

  for (const OptionTarget& target : targets) {
    pb::Option& option = snapshot->options.emplace_back();
    option.text = TargetedOptionText(target);
    option.guid = target.guid;
    option.kind = ToProtoKind(target.kind);
    option.name = target.name;
  }

  // Party assist options (A5): let the brain attack the main assist's or
  // tank's current target without C++ guessing target priority.
  if (alive && snapshot->ext.party) {
    auto find_party_target_guid =
        [&](std::string_view member_name) -> std::optional<std::string_view> {
      for (const OptionFacts::PartyTarget& pt : facts.party_targets) {
        if (pt.member_name == member_name && !pt.target_guid.empty()) {
          return pt.target_guid;
        }
      }
      return std::nullopt;
    };
    std::vector<const pb::Unit*> callers;
    for (const pb::Unit& m : snapshot->ext.party->members) {
      if (m.name == snapshot->bot) {
        continue;
      }
      if (!m.dead && m.group && m.group->online && !m.target.empty() &&
          m.group->main_assist) {
        callers.push_back(&m);
      }
    }
    for (const pb::Unit& m : snapshot->ext.party->members) {
      if (m.name == snapshot->bot) {
        continue;
      }
      if (!m.dead && m.group && m.group->online && !m.target.empty() &&
          !m.group->main_assist &&
          (m.group->main_tank || m.group->role == pb::TANK)) {
        callers.push_back(&m);
      }
    }
    std::vector<std::string> seen_assist_guids;
    for (const pb::Unit* caller : callers) {
      if (seen_assist_guids.size() >= 2) {
        break;
      }
      const std::optional<std::string_view> exact_guid =
          find_party_target_guid(caller->name);
      const pb::Unit* best = nullptr;
      for (const pb::Unit& u : snapshot->base.nearby.units) {
        if (exact_guid) {
          if (u.guid != *exact_guid) {
            continue;
          }
        } else if (u.name != caller->target) {
          continue;
        }
        if (UnitKind(u, self.in_combat, facts) != OptionTarget::Kind::kAttack) {
          continue;
        }
        if (!best || u.rel.dist_yd < best->rel.dist_yd) {
          best = &u;
        }
      }
      if (!best) {
        continue;
      }
      bool dup = false;
      for (const std::string& g : seen_assist_guids) {
        if (g == best->guid) {
          dup = true;
          break;
        }
      }
      if (dup) {
        continue;
      }
      seen_assist_guids.push_back(best->guid);
      const std::string_view role_prefix =
          caller->group->main_assist ? "main assist " : "tank ";
      const std::string assist_name = caller->name + "'s target";
      const std::string text = "Attack " + std::string(role_prefix) +
                               assist_name + " (" + best->name + ")";
      targets.push_back({OptionTarget::Kind::kAttack, best->guid, best->name});
      pb::Option& option = snapshot->options.emplace_back();
      option.text = text;
      // Distinct synthetic guid so PlanRunner::MatchOptionInScreen matches the
      // assist option rather than colliding with the regular Attack <mob> row;
      // OptionTarget::guid above still carries the real unit guid for
      // execution, and option.name carries the mob name so `expect:` matches.
      option.guid = "assist:" + best->guid;
      option.kind = pb::Option::ATTACK;
      option.name = best->name;
    }
  }

  if (alive) {
    for (const OptionFacts::Item& item : facts.usable_items) {
      if (item.entry == 0 || item.name.empty()) {
        continue;
      }
      if (self.in_combat && (item.out_of_combat_only || item.equippable)) {
        continue;
      }
      const std::string item_guid =
          (item.equippable ? "equip:" : "item:") + std::to_string(item.entry);
      targets.push_back({OptionTarget::Kind::kUse, item_guid, item.name});
      pb::Option& option = snapshot->options.emplace_back();
      if (item.equippable) {
        option.text = item.detail.empty()
                          ? "Equip " + item.name
                          : "Equip " + item.name + " (" + item.detail + ")";
      } else {
        option.text = "Use " + item.name;
      }
      option.guid = item_guid;
      option.kind = pb::Option::USE;
      option.name = item.name;
    }
  }

  if (alive && !self.in_combat) {
    const bool hurt = self.health_max > 0 && self.health < self.health_max;
    const bool low_mana = self.power_type == pb::Self::MANA &&
                          self.power_max > 0 && self.power < self.power_max;
    if (!self.mounted && !self.swimming && (hurt || low_mana)) {
      const std::string text = RestText(facts);
      targets.push_back({OptionTarget::Kind::kRest, "", text});
      pb::Option& option = snapshot->options.emplace_back();
      option.text = text;
      option.kind = pb::Option::REST;
    }

    size_t explore_count = 0;
    uint32_t seen_dirs = 0;
    for (const OptionFacts::Spot& spot : facts.spots) {
      if (explore_count >= 3) {
        break;
      }
      const uint32_t dir_bit = 1u << static_cast<uint8_t>(spot.dir);
      if (spot.dir != pb::DIRECTION_UNSPECIFIED && (seen_dirs & dir_bit) != 0) {
        continue;  // At most one spot per compass direction.
      }
      seen_dirs |= dir_bit;
      ++explore_count;
      const std::string text = ExploreText(spot);
      targets.push_back(
          {OptionTarget::Kind::kExplore, spot.id, spot.label, spot.pos});
      pb::Option& option = snapshot->options.emplace_back();
      option.text = text;
      option.guid = spot.id;
      option.kind = pb::Option::EXPLORE;
      option.name =
          !spot.label.empty() ? spot.label : std::string(DirText(spot.dir));
    }

    for (const OptionFacts::Place& place : facts.places) {
      if (place.name.empty()) {
        continue;
      }
      const std::string text = GoToText(place);
      targets.push_back(
          {OptionTarget::Kind::kGoTo, place.id, place.name, place.pos});
      pb::Option& option = snapshot->options.emplace_back();
      option.text = text;
      option.guid = place.id;
      option.kind = pb::Option::GO_TO;
      option.name = place.name;
    }
  }

  if (!facts.pending_group_invite_from.empty()) {
    const std::string& from = facts.pending_group_invite_from;
    targets.push_back({OptionTarget::Kind::kAcceptInvite, "", from});
    pb::Option& acc = snapshot->options.emplace_back();
    acc.text = "Accept group invite from " + from;
    acc.kind = pb::Option::ACCEPT_INVITE;
    acc.name = from;

    targets.push_back({OptionTarget::Kind::kDeclineInvite, "", from});
    pb::Option& dec = snapshot->options.emplace_back();
    dec.text = "Decline group invite from " + from;
    dec.kind = pb::Option::DECLINE_INVITE;
    dec.name = from;
  }
  for (const std::string& player_name : facts.invite_candidates) {
    if (player_name.empty()) {
      continue;
    }
    targets.push_back({OptionTarget::Kind::kInvite, "", player_name});
    pb::Option& inv = snapshot->options.emplace_back();
    inv.text = "Invite " + player_name + " to group";
    inv.kind = pb::Option::INVITE;
    inv.name = player_name;
  }
  if (facts.can_leave_group && !self.in_combat) {
    targets.push_back({OptionTarget::Kind::kLeaveGroup, "", ""});
    pb::Option& lv = snapshot->options.emplace_back();
    lv.text = "Leave group";
    lv.kind = pb::Option::LEAVE_GROUP;
  }

  targets.push_back({OptionTarget::Kind::kKeep, "", ""});
  pb::Option& keep = snapshot->options.emplace_back();
  keep.text = "Keep doing what I'm doing";
  keep.kind = pb::Option::KEEP;
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
  while (!entries_.empty() && built - entries_.front().built > window_) {
    entries_.pop_front();
  }
}

OptionHistory::Lookup OptionHistory::Find(uint32_t seq, uint32_t option) const {
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
    if (it->seq != seq) {
      continue;
    }
    if (option == 0 || option > it->options.size()) {
      return {nullptr, base::TimeTicks(),
              "no option " + std::to_string(option) + " in snapshot " +
                  std::to_string(seq)};
    }
    return {&it->options[option - 1], it->built, ""};
  }
  return {nullptr, base::TimeTicks(),
          "snapshot " + std::to_string(seq) + " is too old or unknown"};
}

}  // namespace llm_brain
