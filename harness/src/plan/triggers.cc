// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "plan/triggers.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <flat_set>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "llm/scheduler.h"
#include "plan/plan.h"
#include "snapshot/snapshot.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

std::string LowerWords(std::string_view enum_name) {
  std::string out(enum_name);
  for (char& c : out) {
    c = c == '_' ? ' ' : base::ToLowerASCII(c);
  }
  return out;
}

bool IsAlive(const pb::Self& self) {
  return self.state != pb::Self::DEAD_BODY && self.state != pb::Self::GHOST;
}

std::optional<double> HealthPercent(const pb::Self& self) {
  if (self.health_max > 0) {
    return 100.0 * static_cast<double>(self.health) /
           static_cast<double>(self.health_max);
  }
  if (self.unit.health_pct) {
    return *self.unit.health_pct;
  }
  return std::nullopt;
}

std::optional<double> ManaPercent(const pb::Self& self) {
  if (self.power_type == pb::Self::MANA && self.power_max > 0) {
    return 100.0 * static_cast<double>(self.power) /
           static_cast<double>(self.power_max);
  }
  if (self.alt_mana_max > 0) {
    return 100.0 * static_cast<double>(self.alt_mana) /
           static_cast<double>(self.alt_mana_max);
  }
  return std::nullopt;
}

std::string AttackerKey(const pb::Unit& unit) {
  return !unit.guid.empty() ? unit.guid : unit.name;
}

void Consider(std::optional<WakeTrigger>* best, WakeTrigger candidate) {
  if (!*best) {
    *best = std::move(candidate);
    return;
  }
  (*best)->urgency = std::min((*best)->urgency, candidate.urgency);
  (*best)->kinds |= candidate.kinds;
  for (std::string& a : candidate.attackers) {
    if (std::find((*best)->attackers.begin(), (*best)->attackers.end(), a) ==
        (*best)->attackers.end()) {
      (*best)->attackers.push_back(std::move(a));
    }
  }
  if (!candidate.reason.empty() &&
      (*best)->reason.find(candidate.reason) == std::string::npos) {
    base::StrAppend(&(*best)->reason, {"; ", candidate.reason});
  }
}

}  // namespace

std::string DescribeEvent(const pb::Event& event) {
  if (event.chat) {
    const pb::ChatEvent& chat = *event.chat;
    const std::string ch = LowerWords(pb::EnumName(chat.channel));
    if (chat.from.empty()) {
      return base::StrCat({"[", ch, "] ", chat.text});
    }
    return base::StrCat({ch, " from ", chat.from, ": ", chat.text});
  }
  if (event.error) {
    return base::StrCat({"error: ", event.error->text});
  }
  if (event.info) {
    return event.info->text;
  }
  if (event.prompt) {
    const pb::PromptEvent& prompt = *event.prompt;
    std::string out =
        base::StrCat({"prompt: ", LowerWords(pb::EnumName(prompt.kind))});
    if (!prompt.from.empty()) {
      base::StrAppend(&out, {" from ", prompt.from});
    }
    if (!prompt.text.empty()) {
      base::StrAppend(&out, {" (", prompt.text, ")"});
    }
    return out;
  }
  if (event.died) {
    return event.died->killer.empty()
               ? "died"
               : base::StrCat({"killed by ", event.died->killer});
  }
  if (event.resurrected) {
    return event.resurrected->by.empty()
               ? "resurrected"
               : base::StrCat({"resurrected by ", event.resurrected->by});
  }
  if (event.level_up) {
    return base::StrCat(
        {"reached level ", base::NumberToString(event.level_up->level)});
  }
  if (event.xp_gained) {
    return base::StrCat(
        {"gained ", base::NumberToString(event.xp_gained->amount), " xp"});
  }
  if (event.money_changed) {
    return base::StrCat({"money ", event.money_changed->delta >= 0 ? "+" : "",
                         base::NumberToString(event.money_changed->delta),
                         "c"});
  }
  if (event.item_received) {
    const pb::Item empty_item;
    const pb::Item& item =
        event.item_received->item ? *event.item_received->item : empty_item;
    const uint32_t count = item.count > 0 ? item.count : 1;
    return base::StrCat(
        {"received ", base::NumberToString(count), "x ", item.name});
  }
  if (event.quest_complete) {
    return base::StrCat({"quest complete: ", event.quest_complete->quest});
  }
  if (event.quest_failed) {
    return base::StrCat({"quest failed: ", event.quest_failed->quest});
  }
  if (event.quest_accepted) {
    return base::StrCat({"quest accepted: ", event.quest_accepted->quest});
  }
  if (event.loot_roll) {
    const std::string_view name =
        event.loot_roll->item ? std::string_view(event.loot_roll->item->name)
                              : std::string_view{};
    return base::StrCat({"loot roll: ", name});
  }
  if (event.path_failed) {
    return "path failed";
  }
  if (event.displaced) {
    const std::string how = LowerWords(pb::EnumName(event.displaced->how));
    return how.empty() || how == "how unspecified" ? "displaced" : how;
  }
  if (event.dbm_warning) {
    return base::StrCat({"dbm warning: ", event.dbm_warning->text});
  }
  if (event.kicked) {
    return base::StrCat({"kicked from ", event.kicked->from});
  }
  if (event.duel) {
    return base::StrCat(
        {"duel ", event.duel->what, " with ", event.duel->with});
  }
  if (event.lfg) {
    return base::StrCat({"lfg: ", event.lfg->what});
  }
  return "";
}

std::optional<WakeTrigger> EvaluateEventTrigger(
    const pb::Event& event,
    const std::vector<Watch>& /*watches*/) {
  if (event.prompt) {
    return WakeTrigger{Urgency::kP1Danger, DescribeEvent(event)};
  }
  if (event.died) {
    return WakeTrigger{Urgency::kP0Combat, DescribeEvent(event)};
  }
  if (event.resurrected) {
    return WakeTrigger{Urgency::kP2Routine, DescribeEvent(event)};
  }
  if (event.displaced) {
    return WakeTrigger{Urgency::kP0Combat, DescribeEvent(event)};
  }
  if (event.dbm_warning) {
    return WakeTrigger{Urgency::kP0Combat, DescribeEvent(event)};
  }
  if (event.loot_roll) {
    return WakeTrigger{Urgency::kP1Danger, DescribeEvent(event)};
  }
  if (event.path_failed) {
    return WakeTrigger{Urgency::kP2Routine, "path failed"};
  }
  if (event.quest_failed) {
    return WakeTrigger{Urgency::kP2Routine, DescribeEvent(event)};
  }
  if (event.kicked) {
    return WakeTrigger{Urgency::kP2Routine, DescribeEvent(event)};
  }
  if (event.error) {
    if (event.error->command_id != 0) {
      return WakeTrigger{Urgency::kP2Routine,
                         base::StrCat({"step failed: ", event.error->text})};
    }
    return std::nullopt;
  }
  if (event.chat) {
    const pb::ChatEvent& chat = *event.chat;
    if (chat.channel == pb::WHISPER || chat.channel == pb::MONSTER_WHISPER) {
      // Direct whispers outrank routine P2 wakes so a human's message is not
      // starved behind routine questing/navigation under load (A-M7).
      return WakeTrigger{Urgency::kP1Danger,
                         base::StrCat({"whisper from ", chat.from})};
    }
    // A boss emote announces a mechanic: P0. A raid warning (harness #20) is a
    // call to every bot of the raid at once: P1, so it never cancels 25
    // in-flight calls.
    if (chat.channel == pb::BOSS_EMOTE) {
      return WakeTrigger{Urgency::kP0Combat, DescribeEvent(event)};
    }
    if (chat.channel == pb::RAID_WARNING) {
      return WakeTrigger{Urgency::kP1Danger, DescribeEvent(event)};
    }
    if (chat.at_me && chat.channel != pb::WHISPER_SENT) {
      const Urgency u = (chat.channel == pb::PARTY || chat.channel == pb::RAID)
                            ? Urgency::kP1Danger
                            : Urgency::kP3Chat;
      return WakeTrigger{u, DescribeEvent(event)};
    }
    return std::nullopt;
  }
  if (event.duel) {
    if (event.duel->what == "started") {
      return WakeTrigger{Urgency::kP0Combat, DescribeEvent(event)};
    }
    return std::nullopt;
  }
  if (event.lfg) {
    if (event.lfg->what == "teleported_in" ||
        event.lfg->what == "dungeon_completed" || event.lfg->what == "kicked") {
      return WakeTrigger{Urgency::kP2Routine, DescribeEvent(event)};
    }
    return std::nullopt;
  }
  return std::nullopt;
}

TriggerEvaluator::WatchInputs TriggerEvaluator::InputsOf(
    const pb::Snapshot& snapshot) {
  WatchInputs in;
  in.alive = IsAlive(snapshot.base.self);
  in.hp_pct = HealthPercent(snapshot.base.self);
  in.mana_pct = ManaPercent(snapshot.base.self);
  for (const pb::Unit& u : snapshot.base.nearby.units) {
    if (!u.dead && u.kind == pb::Unit::PLAYER &&
        u.reaction == pb::Unit::HOSTILE && std::isfinite(u.rel.dist_yd)) {
      in.enemy_players.push_back({AttackerKey(u), u.rel.dist_yd});
    }
  }
  return in;
}

void TriggerEvaluator::SetPlanContext(std::vector<Watch> watches,
                                      std::flat_set<std::string> expected_names,
                                      const WatchInputs* seen) {
  const WatchInputs& in = seen != nullptr ? *seen : last_;
  watches_ = std::move(watches);
  watch_fired_.assign(watches_.size(), false);
  watch_seen_players_.assign(watches_.size(), {});
  for (size_t i = 0; i < watches_.size(); ++i) {
    const Watch& watch = watches_[i];
    const auto value = static_cast<double>(watch.value);
    switch (watch.kind) {
      case Watch::Kind::kHealthBelow:
        watch_fired_[i] = in.alive && in.hp_pct && *in.hp_pct < value;
        break;
      case Watch::Kind::kManaBelow:
        watch_fired_[i] = in.alive && in.mana_pct && *in.mana_pct < value;
        break;
      case Watch::Kind::kEnemyPlayerWithin:
        for (const WatchInputs::EnemyPlayer& p : in.enemy_players) {
          if (p.dist_yd <= static_cast<float>(watch.value)) {
            watch_seen_players_[i].insert(p.key);
          }
        }
        break;
      case Watch::Kind::kAttacked:
      case Watch::Kind::kWhisper:
        break;
    }
  }
  expected_names_ = std::move(expected_names);
}

void TriggerEvaluator::SetExpectedNames(
    std::flat_set<std::string> expected_names) {
  expected_names_ = std::move(expected_names);
}

std::optional<WakeTrigger> TriggerEvaluator::EvaluateSnapshot(
    const pb::Snapshot& snapshot) {
  std::optional<WakeTrigger> chosen;
  const pb::Self& self = snapshot.base.self;
  const bool alive = IsAlive(self);
  const bool in_combat = self.in_combat;
  const pb::Self::Control& c = self.control;
  const ControlFlags control{c.stunned, c.feared,   c.charmed, c.silenced,
                             c.rooted,  c.pacified, c.disarmed};

  bool watch_attacked = false;
  for (const Watch& w : watches_) {
    if (w.kind == Watch::Kind::kAttacked) {
      watch_attacked = true;
    }
  }

  // Collect current attackers (non-friendly units targeting me in nearby, and
  // ext.combat). A friendly unit targeting me (a healer, a party member) is not
  // an attacker.
  std::flat_set<std::string> attackers;
  std::vector<std::pair<std::string, std::string>> attacker_entries;
  for (const pb::Unit& u : snapshot.base.nearby.units) {
    if (!u.dead && u.targeting_me && u.reaction != pb::Unit::FRIENDLY) {
      const std::string key = AttackerKey(u);
      if (attackers.insert(key).second) {
        attacker_entries.push_back({key, u.name});
      }
    }
  }
  if (snapshot.ext.combat) {
    for (const pb::Unit& u : snapshot.ext.combat->attackers) {
      if (!u.dead) {
        const std::string key = AttackerKey(u);
        if (attackers.insert(key).second) {
          attacker_entries.push_back({key, u.name});
        }
      }
    }
  }

  if (seen_snapshot_) {
    if (was_alive_ && !alive) {
      Consider(&chosen, {Urgency::kUrgent, "died", kWakeDied});
    }
    // Back from the dead (resurrected, or the spirit reached the body): while
    // dead only Keep was offered, so there is a choice to make again.
    if (!was_alive_ && alive) {
      Consider(&chosen, {Urgency::kNormal, "alive again", kWakeAlive});
    }
    const struct {
      bool was;
      bool now;
      const char* label;
    } controls[] = {
        {prev_control_.stunned, control.stunned, "stunned"},
        {prev_control_.feared, control.feared, "feared"},
        {prev_control_.charmed, control.charmed, "charmed"},
        {prev_control_.silenced, control.silenced, "silenced"},
        {prev_control_.rooted, control.rooted, "rooted"},
        {prev_control_.pacified, control.pacified, "pacified"},
        {prev_control_.disarmed, control.disarmed, "disarmed"},
    };
    for (const auto& item : controls) {
      if (!item.was && item.now) {
        Consider(&chosen, {Urgency::kUrgent,
                           base::StrCat({"lost control (", item.label, ")"}),
                           kWakeLostControl});
      }
    }

    // New attackers not covered by `expect`.
    bool any_expected_attacker = false;
    for (const auto& [key, name] : attacker_entries) {
      if (expected_names_.count(name) > 0) {
        any_expected_attacker = true;
      }
      if (prev_attackers_.count(key) == 0) {
        if (watch_attacked || expected_names_.count(name) == 0) {
          WakeTrigger wt{
              Urgency::kUrgent,
              name.empty() ? "attacked" : base::StrCat({"attacked by ", name}),
              kWakeAttacked};
          if (!name.empty()) {
            wt.attackers.push_back(name);
          }
          Consider(&chosen, std::move(wt));
        }
      }
    }

    // Entered combat when the plan expected no fight at all.
    if (!was_in_combat_ && in_combat) {
      if (watch_attacked) {
        Consider(&chosen, {Urgency::kUrgent, "attacked", kWakeAttacked});
      } else if (expected_names_.empty() && !any_expected_attacker) {
        Consider(&chosen,
                 {Urgency::kUrgent, "entered combat", kWakeEnteredCombat});
      }
    }
  }

  // The party (T8): a bot wakes on its group's state too, not only its own.
  std::flat_set<std::string> dead_members;
  std::flat_set<std::string> low_mana_healers;
  std::flat_set<std::string> critical_members;
  std::string aggro_member;
  std::flat_set<std::string> online_members;
  std::flat_set<std::string> offline_members;
  if (snapshot.ext.party) {
    for (const pb::Unit& m : snapshot.ext.party->members) {
      if (!m.name.empty() && m.group && !m.group->online) {
        offline_members.insert(m.name);
      }
      if (m.name.empty() || !m.group || !m.group->online) {
        continue;
      }
      online_members.insert(m.name);
      if (m.dead) {
        dead_members.insert(m.name);
      } else if (m.group->aggro && m.group->in_range && aggro_member.empty()) {
        aggro_member = m.name;
      }
      // A member near death in a fight (the bot's own fight, or one
      // the member is taking hits in) is worth a look at once. Latched until
      // the member is back above kPartyCriticalRearmPct (or dies: that has
      // its own wake).
      constexpr float kPartyCriticalHpPct = 35.0f;
      constexpr float kPartyCriticalRearmPct = 50.0f;
      if (!m.dead && m.health_pct) {
        if (*m.health_pct < kPartyCriticalHpPct && m.group->in_range &&
            (in_combat || m.group->aggro)) {
          critical_members.insert(m.name);
        } else if (*m.health_pct < kPartyCriticalRearmPct &&
                   prev_critical_members_.contains(m.name)) {
          critical_members.insert(m.name);  // Still latched.
        }
      }
      constexpr float kHealerLowManaPct = 30.0f;
      if (!m.dead && m.group->role == pb::HEALER && m.power_pct &&
          *m.power_pct < kHealerLowManaPct) {
        low_mana_healers.insert(m.name);
      }
    }
  }
  // A bot that tanks for its group (its role, as the party frame shows it)
  // is told when a mob goes for another member: who started the fight does
  // not matter. Only the fact; the brain picks what to do.
  std::flat_set<std::string> loose_mobs;
  std::vector<std::pair<std::string, std::string>> new_loose;  // (mob, on)
  const std::optional<pb::GroupPart>& my_group = self.unit.group;
  if (snapshot.ext.party && my_group &&
      (my_group->role == pb::TANK || my_group->main_tank)) {
    std::flat_set<std::string> others;  // Non-tank members but me.
    for (const pb::Unit& m : snapshot.ext.party->members) {
      if (!m.name.empty() && m.name != snapshot.bot &&
          !(m.group && (m.group->role == pb::TANK || m.group->main_tank))) {
        others.insert(m.name);
      }
    }
    for (const pb::Unit& u : snapshot.base.nearby.units) {
      if (u.dead || u.crowd_control || u.reaction == pb::Unit::FRIENDLY ||
          u.kind == pb::Unit::PLAYER || !others.contains(u.target)) {
        continue;
      }
      const std::string key = AttackerKey(u);
      if (loose_mobs.insert(key).second && !prev_loose_mobs_.contains(key)) {
        new_loose.push_back({u.name, u.target});
      }
    }
  }

  if (seen_snapshot_) {
    for (const auto& [mob, on] : new_loose) {
      Consider(
          &chosen,
          {Urgency::kP1Danger,
           base::StrCat({"loose mob ", mob.empty() ? "?" : mob, " on ", on}),
           kWakeParty});
    }
    for (const std::string& name : dead_members) {
      if (!prev_dead_members_.contains(name)) {
        Consider(&chosen,
                 {Urgency::kP1Danger,
                  base::StrCat({"party member ", name, " died"}), kWakeParty});
      }
    }
    if (!aggro_member.empty() && !prev_party_aggro_ && !in_combat) {
      Consider(&chosen,
               {Urgency::kP1Danger,
                base::StrCat({"party in combat (", aggro_member, " attacked)"}),
                kWakeParty});
    }
    // T11: a member who leaves or logs out (the human the bots came with
    // most of all) is a choice to make: stay, go on, or leave too.
    for (const std::string& name : prev_online_members_) {
      if (!online_members.contains(name)) {
        Consider(&chosen, {Urgency::kP1Danger,
                           base::StrCat({"party member ", name,
                                         offline_members.contains(name)
                                             ? " went offline"
                                             : " left the group"}),
                           kWakeParty});
      }
    }
    for (const std::string& name : critical_members) {
      if (!prev_critical_members_.contains(name)) {
        Consider(&chosen,
                 {Urgency::kP1Danger,
                  base::StrCat({"party member ", name, " is near death"}),
                  kWakeParty});
      }
    }
    for (const std::string& name : low_mana_healers) {
      if (!prev_low_mana_healers_.contains(name)) {
        Consider(&chosen,
                 {Urgency::kP1Danger,
                  base::StrCat({"healer ", name, " low on mana"}), kWakeParty});
      }
    }
  }

  // Evaluate watches (edge-latched per crossing).
  const std::optional<double> hp_pct = HealthPercent(self);
  const std::optional<double> mana_pct = ManaPercent(self);
  if (alive) {
    for (size_t i = 0; i < watches_.size(); ++i) {
      const Watch& watch = watches_[i];
      switch (watch.kind) {
        case Watch::Kind::kHealthBelow:
          if (hp_pct) {
            if (*hp_pct < static_cast<double>(watch.value)) {
              if (!watch_fired_[i]) {
                watch_fired_[i] = true;
                Consider(
                    &chosen,
                    {Urgency::kUrgent,
                     base::StrCat({"hp<", base::NumberToString(watch.value)}),
                     kWakeHealth});
              }
            } else {
              watch_fired_[i] = false;
            }
          }
          break;
        case Watch::Kind::kManaBelow:
          if (mana_pct) {
            if (*mana_pct < static_cast<double>(watch.value)) {
              if (!watch_fired_[i]) {
                watch_fired_[i] = true;
                Consider(
                    &chosen,
                    {in_combat ? Urgency::kUrgent : Urgency::kNormal,
                     base::StrCat({"mana<", base::NumberToString(watch.value)}),
                     kWakeMana});
              }
            } else {
              watch_fired_[i] = false;
            }
          }
          break;
        case Watch::Kind::kEnemyPlayerWithin: {
          std::flat_set<std::string> current_within;
          for (const pb::Unit& u : snapshot.base.nearby.units) {
            const float dist = u.rel.dist_yd;
            if (!u.dead && u.kind == pb::Unit::PLAYER &&
                u.reaction == pb::Unit::HOSTILE && std::isfinite(dist) &&
                dist <= static_cast<float>(watch.value)) {
              const std::string key = AttackerKey(u);
              current_within.insert(key);
              if (watch_seen_players_[i].count(key) == 0) {
                Consider(
                    &chosen,
                    {Urgency::kP1Danger,
                     base::StrCat({"enemy player ", u.name, " within ",
                                   base::NumberToString(watch.value), " yd"}),
                     kWakeEnemyPlayer});
              }
            }
          }
          watch_seen_players_[i] = std::move(current_within);
          break;
        }
        case Watch::Kind::kAttacked:
        case Watch::Kind::kWhisper:
          break;
      }
    }
  }

  for (const pb::Event& ev : snapshot.events) {
    if (std::optional<WakeTrigger> trig = EvaluateEventTrigger(ev, watches_)) {
      Consider(&chosen, std::move(*trig));
    }
  }

  last_ = InputsOf(snapshot);

  seen_snapshot_ = true;
  was_alive_ = alive;
  was_in_combat_ = in_combat;
  prev_control_ = control;
  prev_attackers_ = std::move(attackers);
  prev_dead_members_ = std::move(dead_members);
  prev_party_aggro_ = !aggro_member.empty();
  prev_low_mana_healers_ = std::move(low_mana_healers);
  prev_critical_members_ = std::move(critical_members);
  prev_online_members_ = std::move(online_members);
  prev_loose_mobs_ = std::move(loose_mobs);
  return chosen;
}

}  // namespace harness
