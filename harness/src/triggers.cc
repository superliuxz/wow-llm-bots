// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "triggers.h"

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
#include "plan.h"
#include "proto/snapshot.h"
#include "scheduler.h"

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
  if (candidate.urgency == Urgency::kUrgent) {
    (*best)->urgency = Urgency::kUrgent;
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
    return WakeTrigger{Urgency::kUrgent, DescribeEvent(event)};
  }
  if (event.died) {
    return WakeTrigger{Urgency::kUrgent, DescribeEvent(event)};
  }
  if (event.resurrected) {
    return WakeTrigger{Urgency::kNormal, DescribeEvent(event)};
  }
  if (event.displaced) {
    return WakeTrigger{Urgency::kUrgent, DescribeEvent(event)};
  }
  if (event.dbm_warning) {
    return WakeTrigger{Urgency::kUrgent, DescribeEvent(event)};
  }
  if (event.loot_roll) {
    return WakeTrigger{Urgency::kUrgent, DescribeEvent(event)};
  }
  if (event.path_failed) {
    return WakeTrigger{Urgency::kNormal, "path failed"};
  }
  if (event.quest_failed) {
    return WakeTrigger{Urgency::kNormal, DescribeEvent(event)};
  }
  if (event.kicked) {
    return WakeTrigger{Urgency::kNormal, DescribeEvent(event)};
  }
  if (event.error) {
    if (event.error->command_id != 0) {
      return WakeTrigger{Urgency::kNormal,
                         base::StrCat({"step failed: ", event.error->text})};
    }
    return std::nullopt;
  }
  if (event.chat) {
    const pb::ChatEvent& chat = *event.chat;
    if (chat.channel == pb::WHISPER || chat.channel == pb::MONSTER_WHISPER) {
      return WakeTrigger{Urgency::kNormal,
                         base::StrCat({"whisper from ", chat.from})};
    }
    if (chat.channel == pb::RAID_WARNING || chat.channel == pb::BOSS_EMOTE) {
      return WakeTrigger{Urgency::kUrgent, DescribeEvent(event)};
    }
    if (chat.at_me && chat.channel != pb::WHISPER_SENT) {
      return WakeTrigger{Urgency::kNormal, DescribeEvent(event)};
    }
    return std::nullopt;
  }
  if (event.duel) {
    if (event.duel->what == "started") {
      return WakeTrigger{Urgency::kUrgent, DescribeEvent(event)};
    }
    return std::nullopt;
  }
  if (event.lfg) {
    if (event.lfg->what == "teleported_in" ||
        event.lfg->what == "dungeon_completed" || event.lfg->what == "kicked") {
      return WakeTrigger{Urgency::kNormal, DescribeEvent(event)};
    }
    return std::nullopt;
  }
  return std::nullopt;
}

void TriggerEvaluator::SetPlanContext(
    std::vector<Watch> watches,
    std::flat_set<std::string> expected_names) {
  watches_ = std::move(watches);
  watch_fired_.assign(watches_.size(), false);
  watch_seen_players_.assign(watches_.size(), {});
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

  // Collect current attackers (units targeting me in nearby or ext.combat).
  std::flat_set<std::string> attackers;
  std::vector<std::pair<std::string, std::string>> attacker_entries;
  for (const pb::Unit& u : snapshot.base.nearby.units) {
    if (!u.dead && u.targeting_me) {
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
      Consider(&chosen, {Urgency::kUrgent, "died"});
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
                           base::StrCat({"lost control (", item.label, ")"})});
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
          Consider(&chosen,
                   {Urgency::kUrgent,
                    name.empty() ? "attacked"
                                 : base::StrCat({"attacked by ", name})});
        }
      }
    }

    // Entered combat when the plan expected no fight at all.
    if (!was_in_combat_ && in_combat) {
      if (watch_attacked) {
        Consider(&chosen, {Urgency::kUrgent, "attacked"});
      } else if (expected_names_.empty() && !any_expected_attacker) {
        Consider(&chosen, {Urgency::kUrgent, "entered combat"});
      }
    }
  }

  // Evaluate watches (edge-latched per crossing).
  if (alive) {
    const std::optional<double> hp_pct = HealthPercent(self);
    const std::optional<double> mana_pct = ManaPercent(self);
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
                     base::StrCat({"hp<", base::NumberToString(watch.value)})});
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
                Consider(&chosen,
                         {in_combat ? Urgency::kUrgent : Urgency::kNormal,
                          base::StrCat(
                              {"mana<", base::NumberToString(watch.value)})});
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
                    {Urgency::kUrgent,
                     base::StrCat({"enemy player ", u.name, " within ",
                                   base::NumberToString(watch.value), " yd"})});
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

  seen_snapshot_ = true;
  was_alive_ = alive;
  was_in_combat_ = in_combat;
  prev_control_ = control;
  prev_attackers_ = std::move(attackers);
  return chosen;
}

}  // namespace harness
