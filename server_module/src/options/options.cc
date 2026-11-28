// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "options/options.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "options/action_types.h"
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
      return (npc.npc_flags & (kTalkNpcFlags | kNpcFlagHasDialogue)) != 0;
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

BotAction MakeTargetAction(OptionTarget::Kind kind,
                           const std::string& guid,
                           const std::string& name) {
  switch (kind) {
    case OptionTarget::Kind::kAttack:
      return AttackAction{guid, name};
    case OptionTarget::Kind::kLoot:
      return LootAction{guid, name};
    case OptionTarget::Kind::kGather:
      return GatherAction{guid, name};
    case OptionTarget::Kind::kUse:
      return UseObjectAction{guid, name};
    case OptionTarget::Kind::kTalk:
      return TalkAction{guid, name};
    default:
      return KeepAction{};
  }
}

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
        (*targets)[i].action = MakeTargetAction(kind, guid, name);
        (*ranks)[i] = rank;
      }
      return;
    }
  }
  targets->push_back(
      {kind, guid, name, std::nullopt, MakeTargetAction(kind, guid, name)});
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
  constexpr std::string_view kTurnInPrefix = "turn in: ";
  std::string text;
  if (place.id.starts_with("place:quest:") && place.id.ends_with(":turnin") &&
      place.services.starts_with(kTurnInPrefix)) {
    text = "Turn in " + place.services.substr(kTurnInPrefix.size()) + " at " +
           place.name;
  } else {
    text = "Go to " + place.name;
    if (!place.services.empty()) {
      text += " (" + place.services + ")";
    }
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

namespace {

const char* MarkerName(pb::RaidMarker m) {
  switch (m) {
    case pb::STAR:
      return "star";
    case pb::CIRCLE:
      return "circle";
    case pb::DIAMOND:
      return "diamond";
    case pb::TRIANGLE:
      return "triangle";
    case pb::MOON:
      return "moon";
    case pb::SQUARE:
      return "square";
    case pb::CROSS:
      return "cross";
    case pb::SKULL:
      return "skull";
    default:
      return "";
  }
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
  auto has_explicit_npc_actions = [&](std::string_view npc_guid) {
    for (const OptionFacts::NpcAction& act : facts.npc_actions) {
      if (act.npc_guid == npc_guid) {
        return true;
      }
    }
    return false;
  };
  for (const pb::Unit& u : snapshot->base.nearby.units) {
    if (const std::optional<OptionTarget::Kind> kind =
            UnitKind(u, self.in_combat, facts);
        kind && alive) {
      if (*kind == OptionTarget::Kind::kTalk &&
          has_explicit_npc_actions(u.guid)) {
        continue;
      }
      // The leader's kill marks win over nearness: among mobs of one name,
      // the skull (or cross) is the one to attack.
      const bool kill_mark =
          u.raid_marker == pb::SKULL || u.raid_marker == pb::CROSS;
      AddTarget(
          *kind, u.guid, u.name,
          kill_mark ? Rank{true, -1.0f} : Rank{u.targeting_me, u.rel.dist_yd},
          &targets, &ranks);
    }
  }
  for (const pb::GameObject& go : snapshot->base.nearby.objects) {
    if (const std::optional<OptionTarget::Kind> kind = ObjectKind(go, facts);
        kind && alive) {
      if (*kind == OptionTarget::Kind::kUse && !self.in_combat &&
          has_explicit_npc_actions(go.guid)) {
        continue;
      }
      AddTarget(*kind, go.guid, go.name, {false, go.rel.dist_yd}, &targets,
                &ranks);
    }
  }

  // In a party, say which mobs nobody in it is fighting yet (RH-H2): a fact a
  // player reads off the nameplates, so the brain can leave pulls to the tank.
  std::vector<std::string> party_names;
  if (snapshot->ext.party) {
    for (const pb::Unit& m : snapshot->ext.party->members) {
      party_names.push_back(m.name);
    }
  }
  const auto unit_not_pulled = [&](const pb::Unit& u) {
    if (party_names.empty()) {
      return false;
    }
    const bool fights_party = u.targeting_me || u.target == snapshot->bot ||
                              std::find(party_names.begin(), party_names.end(),
                                        u.target) != party_names.end();
    return !fights_party;
  };
  const auto not_pulled = [&](const OptionTarget& t) {
    if (party_names.empty() || t.kind != OptionTarget::Kind::kAttack) {
      return false;
    }
    for (const pb::Unit& u : snapshot->base.nearby.units) {
      if (u.guid == t.guid) {
        return unit_not_pulled(u);
      }
    }
    return false;
  };
  // Marked mobs come first, skull then cross (kill order), and say their mark
  // (Now #4: follow the leader's marks).
  const auto marker_of = [&](const OptionTarget& t) {
    if (t.kind != OptionTarget::Kind::kAttack) {
      return pb::RAID_MARKER_UNSPECIFIED;
    }
    for (const pb::Unit& u : snapshot->base.nearby.units) {
      if (u.guid == t.guid) {
        return u.raid_marker;
      }
    }
    return pb::RAID_MARKER_UNSPECIFIED;
  };
  const auto kill_rank = [&](const OptionTarget& t) {
    const pb::RaidMarker m = marker_of(t);
    return m == pb::SKULL ? 0 : m == pb::CROSS ? 1 : 2;
  };
  std::stable_sort(targets.begin(), targets.end(),
                   [&](const OptionTarget& a, const OptionTarget& b) {
                     return kill_rank(a) < kill_rank(b);
                   });
  // Pulls wait for the human (who leads, tanks, or is AFK or behind): mobs
  // nobody in the party fights yet get no Attack option at all (a gate, not
  // a prompt rule).
  if (facts.hold_pulls) {
    std::erase_if(targets, not_pulled);
  }
  for (const OptionTarget& target : targets) {
    pb::Option& option = snapshot->options.emplace_back();
    option.text = TargetedOptionText(target);
    if (const pb::RaidMarker m = marker_of(target);
        m != pb::RAID_MARKER_UNSPECIFIED) {
      option.text += std::string(" (") + MarkerName(m) + ")";
    }
    if (not_pulled(target)) {
      option.text += " (not pulled)";
    }
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
    // The party leader's target too (RH-H2): in a group the leader formed by
    // hand nobody may be flagged tank, yet followers assist the leader.
    for (const pb::Unit& m : snapshot->ext.party->members) {
      if (m.name != snapshot->bot && !m.dead && m.group && m.group->online &&
          !m.target.empty() && m.group->leader &&
          std::find(callers.begin(), callers.end(), &m) == callers.end()) {
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
      const bool assist_not_pulled = unit_not_pulled(*best);
      if (facts.hold_pulls && assist_not_pulled) {
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
          caller->group->main_assist ? "main assist "
          : (caller->group->main_tank || caller->group->role == pb::TANK)
              ? "tank "
              : "leader ";
      const std::string assist_name = caller->name + "'s target";
      std::string text = "Attack " + std::string(role_prefix) + assist_name +
                         " (" + best->name + ")";
      if (assist_not_pulled) {
        text += " (not pulled)";
      }
      targets.push_back({OptionTarget::Kind::kAttack, best->guid, best->name,
                         std::nullopt, AttackAction{best->guid, best->name}});
      pb::Option& option = snapshot->options.emplace_back();
      option.text = std::move(text);
      // Distinct synthetic guid so PlanRunner::MatchOptionInScreen matches the
      // assist option rather than colliding with the regular Attack <mob> row;
      // OptionTarget::guid above still carries the real unit guid for
      // execution, and option.name carries the mob name so `expect:` matches.
      option.guid = "assist:" + best->guid;
      option.kind = pb::Option::ATTACK;
      option.name = best->name;
    }
  }

  if (alive && !self.in_combat) {
    size_t explore_count = 0;
    const size_t max_explore = facts.hold_explore ? 0 : 3;
    uint32_t seen_dirs = 0;
    for (const OptionFacts::Spot& spot : facts.spots) {
      if (explore_count >= max_explore) {
        break;
      }
      const uint32_t dir_bit = 1u << static_cast<uint8_t>(spot.dir);
      if (spot.dir != pb::DIRECTION_UNSPECIFIED && (seen_dirs & dir_bit) != 0) {
        continue;  // At most one spot per compass direction.
      }
      seen_dirs |= dir_bit;
      ++explore_count;
      const std::string text = ExploreText(spot);
      targets.push_back({OptionTarget::Kind::kExplore, spot.id, spot.label,
                         spot.pos,
                         WalkAction{spot.id, spot.label, spot.pos, true}});
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
      targets.push_back({OptionTarget::Kind::kGoTo, place.id, place.name,
                         place.pos,
                         WalkAction{place.id, place.name, place.pos, false}});
      pb::Option& option = snapshot->options.emplace_back();
      option.text = text;
      option.guid = place.id;
      option.kind = pb::Option::GO_TO;
      option.name = place.name;
    }

    const bool hurt = self.health_max > 0 && self.health < self.health_max;
    const bool low_mana = self.power_type == pb::Self::MANA &&
                          self.power_max > 0 && self.power < self.power_max;
    if (!self.mounted && !self.swimming && (hurt || low_mana)) {
      const std::string text = RestText(facts);
      targets.push_back(
          {OptionTarget::Kind::kRest, "", text, std::nullopt, RestAction{}});
      pb::Option& option = snapshot->options.emplace_back();
      option.text = text;
      option.kind = pb::Option::REST;
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
      BotAction item_action =
          item.equippable ? BotAction{EquipItemAction{item.entry, item.name}}
                          : BotAction{UseBagItemAction{item.entry, item.name}};
      targets.push_back({OptionTarget::Kind::kUse, item_guid, item.name,
                         std::nullopt, std::move(item_action)});
      pb::Option& option = snapshot->options.emplace_back();
      if (item.equippable) {
        option.text = item.detail.empty()
                          ? "Equip " + item.name
                          : "Equip " + item.name + " (" + item.detail + ")";
      } else {
        option.text = item.detail.empty()
                          ? "Use " + item.name
                          : "Use " + item.name + " (" + item.detail + ")";
      }
      option.guid = item_guid;
      option.kind = pb::Option::USE;
      option.name = item.name;
    }
  }

  if (alive && !self.in_combat) {
    for (const OptionFacts::NpcAction& act : facts.npc_actions) {
      if (act.action_guid.empty() || act.label.empty()) {
        continue;
      }
      targets.push_back({OptionTarget::Kind::kUse, act.action_guid, act.label,
                         std::nullopt,
                         DecodeOptionFactAction(act.action_guid, act.label)});
      pb::Option& option = snapshot->options.emplace_back();
      option.text = act.label;
      option.guid = act.action_guid;
      option.kind = pb::Option::USE;
      option.name = act.label;
    }
  }

  // Grouping and upkeep actions (loot rolls, ready check, shared quest, LFG,
  // follow/regroup, raid marking, mounting). Loot rolls and ready checks are
  // valid even while in combat or dead; non-combat upkeep options are filtered
  // by `CollectGroupAndUpkeepActions`.
  for (const OptionFacts::Action& act : facts.group_and_upkeep_actions) {
    if (act.guid.empty() || act.label.empty()) {
      continue;
    }
    BotAction decoded = DecodeOptionFactAction(act.guid, act.label);
    if ((std::holds_alternative<DestroyItemAction>(decoded) ||
         std::holds_alternative<AbandonQuestAction>(decoded) ||
         std::holds_alternative<ItemQuestAction>(decoded)) &&
        (!alive || self.in_combat)) {
      continue;
    }
    targets.push_back({OptionTarget::Kind::kUse, act.guid, act.label,
                       std::nullopt, std::move(decoded)});
    pb::Option& option = snapshot->options.emplace_back();
    option.text = act.label;
    option.guid = act.guid;
    option.kind = pb::Option::USE;
    option.name = act.label;
  }

  if (alive && !self.in_combat) {
    for (const OptionFacts::TalentTree& tree : facts.talent_trees) {
      if (tree.name.empty() || tree.free_points == 0) {
        continue;
      }
      const std::string guid = "talent:" + std::to_string(tree.tab);
      const std::string text = "Spend talent points in " + tree.name + " (" +
                               std::to_string(tree.spent) + " pts, " +
                               std::to_string(tree.free_points) + " unspent)";
      targets.push_back({OptionTarget::Kind::kUse, guid, tree.name,
                         std::nullopt,
                         SpendTalentsAction{tree.tab, tree.name}});
      pb::Option& option = snapshot->options.emplace_back();
      option.text = text;
      option.guid = guid;
      option.kind = pb::Option::USE;
      option.name = tree.name;
    }
  }

  if (self.state == pb::Self::DEAD_BODY) {
    if (facts.has_pending_resurrect) {
      targets.push_back({OptionTarget::Kind::kUse, "rez:accept",
                         "Accept resurrection", std::nullopt,
                         ResurrectAction{ResurrectAction::Mode::kAccept}});
      pb::Option& acc = snapshot->options.emplace_back();
      acc.text = "Accept resurrection";
      acc.guid = "rez:accept";
      acc.kind = pb::Option::USE;
      acc.name = "Accept resurrection";
    }
    // Now #6: a soulstone or ankh up means the bot can get back up on its
    // own, best once the fight is over.
    if (!facts.self_resurrect.empty()) {
      // The soulstone's own spell is already called "Use Soulstone".
      const std::string text =
          (facts.self_resurrect.starts_with("Use ") ? "" : "Use ") +
          facts.self_resurrect + " (resurrect yourself)";
      targets.push_back({OptionTarget::Kind::kUse, "rez:self", text,
                         std::nullopt,
                         ResurrectAction{ResurrectAction::Mode::kSelf}});
      pb::Option& self_rez = snapshot->options.emplace_back();
      self_rez.text = text;
      self_rez.guid = "rez:self";
      self_rez.kind = pb::Option::USE;
      self_rez.name = facts.self_resurrect;
    }
    targets.push_back({OptionTarget::Kind::kUse, "rez:release",
                       "Release spirit", std::nullopt,
                       ResurrectAction{ResurrectAction::Mode::kRelease}});
    pb::Option& rel = snapshot->options.emplace_back();
    rel.text = "Release spirit";
    rel.guid = "rez:release";
    rel.kind = pb::Option::USE;
    rel.name = "Release spirit";
    // RH-M3: how long to lie dead hoping for a rez is the brain's call, as
    // long as at least one online living party member could still resurrect
    // (on a full wipe, nobody alive can rez, so only Release spirit applies).
    const bool has_living_party_member =
        snapshot->ext.party &&
        std::any_of(snapshot->ext.party->members.begin(),
                    snapshot->ext.party->members.end(), [](const pb::Unit& m) {
                      return !m.dead && (!m.group || m.group->online);
                    });
    if (!facts.has_pending_resurrect && has_living_party_member) {
      targets.push_back({OptionTarget::Kind::kUse, "rez:wait",
                         "Wait for a resurrection (up to 5 min)", std::nullopt,
                         ResurrectAction{ResurrectAction::Mode::kWait}});
      pb::Option& wait = snapshot->options.emplace_back();
      wait.text = "Wait for a resurrection (up to 5 min)";
      wait.guid = "rez:wait";
      wait.kind = pb::Option::USE;
      wait.name = "Wait for a resurrection";
    }
  }

  if (!facts.pending_group_invite_from.empty()) {
    const std::string& from = facts.pending_group_invite_from;
    targets.push_back({OptionTarget::Kind::kAcceptInvite, "", from,
                       std::nullopt, AcceptInviteAction{from}});
    pb::Option& acc = snapshot->options.emplace_back();
    acc.text = "Accept group invite from " + from;
    acc.kind = pb::Option::ACCEPT_INVITE;
    acc.name = from;

    targets.push_back({OptionTarget::Kind::kDeclineInvite, "", from,
                       std::nullopt, DeclineInviteAction{from}});
    pb::Option& dec = snapshot->options.emplace_back();
    dec.text = "Decline group invite from " + from;
    dec.kind = pb::Option::DECLINE_INVITE;
    dec.name = from;
  }
  for (const std::string& player_name : facts.invite_candidates) {
    if (player_name.empty()) {
      continue;
    }
    targets.push_back({OptionTarget::Kind::kInvite, "", player_name,
                       std::nullopt, InviteAction{player_name}});
    pb::Option& inv = snapshot->options.emplace_back();
    inv.text = "Invite " + player_name + " to group";
    inv.kind = pb::Option::INVITE;
    inv.name = player_name;
  }
  if (facts.can_leave_group && !self.in_combat) {
    targets.push_back({OptionTarget::Kind::kLeaveGroup, "", "", std::nullopt,
                       LeaveGroupAction{}});
    pb::Option& lv = snapshot->options.emplace_back();
    lv.text = "Leave group";
    lv.kind = pb::Option::LEAVE_GROUP;
  }

  targets.push_back(
      {OptionTarget::Kind::kKeep, "", "", std::nullopt, KeepAction{}});
  pb::Option& keep = snapshot->options.emplace_back();
  keep.text = "Keep doing what I'm doing";
  keep.kind = pb::Option::KEEP;
  return targets;
}

void CollectTauntActions(const pb::Snapshot& snapshot,
                         uint32_t taunt_spell_id,
                         bool not_tank,
                         std::vector<pb::OptionFacts_Action>* out) {
  if (!out || taunt_spell_id == 0 || not_tank) {
    return;
  }
  const pb::Self& self = snapshot.base.self;
  if (self.state == pb::Self::DEAD_BODY || self.state == pb::Self::GHOST) {
    return;
  }
  if (!snapshot.ext.party || !self.unit.group ||
      !(self.unit.group->role == pb::TANK || self.unit.group->main_tank)) {
    return;
  }
  std::vector<const pb::Unit*> candidates;
  for (const pb::Unit& u : snapshot.base.nearby.units) {
    if (u.dead || u.guid.empty() || u.name.empty() || u.kind != pb::Unit::NPC ||
        (u.reaction != pb::Unit::HOSTILE && u.reaction != pb::Unit::NEUTRAL) ||
        u.taunt_immune || u.crowd_control || u.targeting_me ||
        u.target.empty() || !std::isfinite(u.rel.dist_yd) ||
        u.rel.dist_yd > 40.0f) {
      continue;
    }
    candidates.push_back(&u);
  }
  std::stable_sort(
      candidates.begin(), candidates.end(),
      [](const pb::Unit* a, const pb::Unit* b) {
        const bool a_boss = a->classification == pb::Unit::WORLD_BOSS;
        const bool b_boss = b->classification == pb::Unit::WORLD_BOSS;
        if (a_boss != b_boss) {
          return a_boss;
        }
        return a->rel.dist_yd < b->rel.dist_yd;
      });
  constexpr size_t kMaxTauntOptions = 2;
  std::vector<std::string_view> seen_names;
  for (const pb::Unit* u : candidates) {
    if (seen_names.size() >= kMaxTauntOptions) {
      break;
    }
    if (std::find(seen_names.begin(), seen_names.end(), u->name) !=
        seen_names.end()) {
      continue;
    }
    seen_names.push_back(u->name);
    out->push_back({"taunt:" + std::to_string(taunt_spell_id) + ":" + u->guid,
                    "Taunt " + u->name});
  }
}

std::string EscortStartHoldReason(
    bool self_is_leader,
    const std::vector<EscortPartyMember>& members) {
  if (members.empty()) {
    return "";
  }
  for (const EscortPartyMember& m : members) {
    if (m.has_quest) {
      return "";  // Already started by a party member: accept right away.
    }
  }
  for (const EscortPartyMember& m : members) {
    if (m.is_human && m.online) {
      return "wait for your party to start this escort quest";
    }
  }
  if (!self_is_leader) {
    return "wait for the party leader to start this escort quest";
  }
  constexpr float kEscortReadyDistYd = 30.0f;
  for (const EscortPartyMember& m : members) {
    if (!m.online) {
      continue;
    }
    if (!m.alive || !m.same_map || m.dist_to_npc_yd > kEscortReadyDistYd ||
        m.missing_prereq) {
      return "wait until everyone in your party is here and ready for the "
             "escort";
    }
  }
  return "";
}

namespace {

std::vector<LfgRoleChoice> ClassLfgRoleChoices(uint8_t class_id) {
  switch (class_id) {
    case 1:  // CLASS_WARRIOR
    case 6:  // CLASS_DEATH_KNIGHT
      return {{kLfgRoleTank, "Tank"}, {kLfgRoleDamage, "DPS"}};
    case 5:  // CLASS_PRIEST
    case 7:  // CLASS_SHAMAN
      return {{kLfgRoleHealer, "Healer"}, {kLfgRoleDamage, "DPS"}};
    case 2:   // CLASS_PALADIN
    case 11:  // CLASS_DRUID
      return {{kLfgRoleTank, "Tank"},
              {kLfgRoleHealer, "Healer"},
              {kLfgRoleDamage, "DPS"}};
    default:
      return {{kLfgRoleDamage, "DPS"}};
  }
}

}  // namespace

std::vector<LfgRoleChoice> SpecLfgRoleChoices(uint8_t class_id,
                                              uint8_t spec_tab,
                                              bool has_talents,
                                              uint8_t level) {
  if (!has_talents || level < 10) {
    return ClassLfgRoleChoices(class_id);
  }
  const LfgRoleChoice tank{kLfgRoleTank, "Tank"};
  const LfgRoleChoice healer{kLfgRoleHealer, "Healer"};
  const LfgRoleChoice dps{kLfgRoleDamage, "DPS"};
  switch (class_id) {
    case 1:  // Warrior: 0 Arms, 1 Fury, 2 Protection.
      return {spec_tab == 2 ? tank : dps};
    case 2:  // Paladin: 0 Holy, 1 Protection, 2 Retribution.
      if (spec_tab == 0) {
        return {healer};
      }
      return {spec_tab == 1 ? tank : dps};
    case 5:  // Priest: 0 Discipline, 1 Holy, 2 Shadow.
      return {spec_tab == 2 ? dps : healer};
    case 6:  // Death Knight: 0 Blood, 1 Frost, 2 Unholy.
      return spec_tab == 0 ? std::vector<LfgRoleChoice>{tank, dps}
                           : std::vector<LfgRoleChoice>{dps, tank};
    case 7:  // Shaman: 0 Elemental, 1 Enhancement, 2 Restoration.
      return {spec_tab == 2 ? healer : dps};
    case 11:  // Druid: 0 Balance, 1 Feral, 2 Restoration.
      if (spec_tab == 2) {
        return {healer};
      }
      if (spec_tab == 1) {
        return {tank, dps};
      }
      return {dps};
    default:
      return {dps};
  }
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
