// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "triggers.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "plan.h"
#include "proto/snapshot.pb.h"
#include "scheduler.h"

namespace harness {
namespace {

namespace pb = wlb::snapshot;

std::string LowerWords(std::string_view enum_name) {
  std::string out(enum_name);
  for (char& c : out) {
    c = c == '_' ? ' ' : absl::ascii_tolower(c);
  }
  return out;
}

bool IsAlive(const pb::Self& self) {
  return self.state() != pb::Self::DEAD_BODY && self.state() != pb::Self::GHOST;
}

std::optional<double> HealthPercent(const pb::Self& self) {
  if (self.health_max() > 0) {
    return 100.0 * static_cast<double>(self.health()) /
           static_cast<double>(self.health_max());
  }
  if (self.unit().has_health_pct()) {
    return self.unit().health_pct();
  }
  return std::nullopt;
}

std::optional<double> ManaPercent(const pb::Self& self) {
  if (self.power_type() == pb::Self::MANA && self.power_max() > 0) {
    return 100.0 * static_cast<double>(self.power()) /
           static_cast<double>(self.power_max());
  }
  if (self.alt_mana_max() > 0) {
    return 100.0 * static_cast<double>(self.alt_mana()) /
           static_cast<double>(self.alt_mana_max());
  }
  return std::nullopt;
}

std::string AttackerKey(const pb::Unit& unit) {
  return !unit.guid().empty() ? std::string(unit.guid())
                              : std::string(unit.name());
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
    absl::StrAppend(&(*best)->reason, "; ", candidate.reason);
  }
}

}  // namespace

std::string DescribeEvent(const pb::Event& event) {
  switch (event.kind_case()) {
    case pb::Event::kChat: {
      const pb::ChatEvent& chat = event.chat();
      const std::string ch = LowerWords(pb::ChatChannel_Name(chat.channel()));
      if (chat.from().empty()) {
        return absl::StrCat("[", ch, "] ", chat.text());
      }
      return absl::StrCat(ch, " from ", chat.from(), ": ", chat.text());
    }
    case pb::Event::kError:
      return absl::StrCat("error: ", event.error().text());
    case pb::Event::kInfo:
      return std::string(event.info().text());
    case pb::Event::kPrompt: {
      const pb::PromptEvent& prompt = event.prompt();
      std::string out = absl::StrCat(
          "prompt: ", LowerWords(pb::PromptEvent_Kind_Name(prompt.kind())));
      if (!prompt.from().empty()) {
        absl::StrAppend(&out, " from ", prompt.from());
      }
      if (!prompt.text().empty()) {
        absl::StrAppend(&out, " (", prompt.text(), ")");
      }
      return out;
    }
    case pb::Event::kDied:
      return event.died().killer().empty()
                 ? "died"
                 : absl::StrCat("killed by ", event.died().killer());
    case pb::Event::kResurrected:
      return event.resurrected().by().empty()
                 ? "resurrected"
                 : absl::StrCat("resurrected by ", event.resurrected().by());
    case pb::Event::kLevelUp:
      return absl::StrCat("reached level ", event.level_up().level());
    case pb::Event::kXpGained:
      return absl::StrCat("gained ", event.xp_gained().amount(), " xp");
    case pb::Event::kMoneyChanged:
      return absl::StrCat("money ",
                          event.money_changed().delta() >= 0 ? "+" : "",
                          event.money_changed().delta(), "c");
    case pb::Event::kItemReceived: {
      const uint32_t count = event.item_received().item().count() > 0
                                 ? event.item_received().item().count()
                                 : 1;
      return absl::StrCat("received ", count, "x ",
                          event.item_received().item().name());
    }
    case pb::Event::kQuestComplete:
      return absl::StrCat("quest complete: ", event.quest_complete().quest());
    case pb::Event::kQuestFailed:
      return absl::StrCat("quest failed: ", event.quest_failed().quest());
    case pb::Event::kQuestAccepted:
      return absl::StrCat("quest accepted: ", event.quest_accepted().quest());
    case pb::Event::kLootRoll:
      return absl::StrCat("loot roll: ", event.loot_roll().item().name());
    case pb::Event::kPathFailed:
      return "path failed";
    case pb::Event::kDisplaced: {
      const std::string how =
          LowerWords(pb::DisplacedEvent_How_Name(event.displaced().how()));
      return how.empty() || how == "how unspecified" ? "displaced" : how;
    }
    case pb::Event::kDbmWarning:
      return absl::StrCat("dbm warning: ", event.dbm_warning().text());
    case pb::Event::kKicked:
      return absl::StrCat("kicked from ", event.kicked().from());
    case pb::Event::kDuel:
      return absl::StrCat("duel ", event.duel().what(), " with ",
                          event.duel().with());
    case pb::Event::kLfg:
      return absl::StrCat("lfg: ", event.lfg().what());
    default:
      return "";
  }
}

std::optional<WakeTrigger> EvaluateEventTrigger(
    const pb::Event& event,
    const std::vector<Watch>& /*watches*/) {
  switch (event.kind_case()) {
    case pb::Event::kPrompt:
      return WakeTrigger{Urgency::kUrgent, DescribeEvent(event)};
    case pb::Event::kDied:
      return WakeTrigger{Urgency::kUrgent, DescribeEvent(event)};
    case pb::Event::kResurrected:
      return WakeTrigger{Urgency::kNormal, DescribeEvent(event)};
    case pb::Event::kDisplaced:
      return WakeTrigger{Urgency::kUrgent, DescribeEvent(event)};
    case pb::Event::kDbmWarning:
      return WakeTrigger{Urgency::kUrgent, DescribeEvent(event)};
    case pb::Event::kLootRoll:
      return WakeTrigger{Urgency::kUrgent, DescribeEvent(event)};
    case pb::Event::kPathFailed:
      return WakeTrigger{Urgency::kNormal, "path failed"};
    case pb::Event::kQuestFailed:
      return WakeTrigger{Urgency::kNormal, DescribeEvent(event)};
    case pb::Event::kKicked:
      return WakeTrigger{Urgency::kNormal, DescribeEvent(event)};
    case pb::Event::kError:
      if (event.error().command_id() != 0) {
        return WakeTrigger{Urgency::kNormal,
                           absl::StrCat("step failed: ", event.error().text())};
      }
      return std::nullopt;
    case pb::Event::kChat: {
      const pb::ChatEvent& chat = event.chat();
      if (chat.channel() == pb::WHISPER ||
          chat.channel() == pb::MONSTER_WHISPER) {
        return WakeTrigger{Urgency::kNormal,
                           absl::StrCat("whisper from ", chat.from())};
      }
      if (chat.channel() == pb::RAID_WARNING ||
          chat.channel() == pb::BOSS_EMOTE) {
        return WakeTrigger{Urgency::kUrgent, DescribeEvent(event)};
      }
      if (chat.at_me() && chat.channel() != pb::WHISPER_SENT) {
        return WakeTrigger{Urgency::kNormal, DescribeEvent(event)};
      }
      return std::nullopt;
    }
    case pb::Event::kDuel:
      if (event.duel().what() == "started") {
        return WakeTrigger{Urgency::kUrgent, DescribeEvent(event)};
      }
      return std::nullopt;
    case pb::Event::kLfg:
      if (event.lfg().what() == "teleported_in" ||
          event.lfg().what() == "dungeon_completed" ||
          event.lfg().what() == "kicked") {
        return WakeTrigger{Urgency::kNormal, DescribeEvent(event)};
      }
      return std::nullopt;
    default:
      return std::nullopt;
  }
}

void TriggerEvaluator::SetPlanContext(std::vector<Watch> watches,
                                      std::set<std::string> expected_names) {
  watches_ = std::move(watches);
  watch_fired_.assign(watches_.size(), false);
  watch_seen_players_.assign(watches_.size(), {});
  expected_names_ = std::move(expected_names);
}

std::optional<WakeTrigger> TriggerEvaluator::EvaluateSnapshot(
    const pb::Snapshot& snapshot) {
  std::optional<WakeTrigger> chosen;
  const pb::Self& self = snapshot.base().self();
  const bool alive = IsAlive(self);
  const bool in_combat = self.in_combat();
  const pb::Self::Control& c = self.control();
  const ControlFlags control{c.stunned(),  c.feared(), c.charmed(),
                             c.silenced(), c.rooted(), c.pacified(),
                             c.disarmed()};

  bool watch_attacked = false;
  for (const Watch& w : watches_) {
    if (w.kind == Watch::Kind::kAttacked) {
      watch_attacked = true;
    }
  }

  // Collect current attackers (units targeting me in nearby or ext.combat).
  std::set<std::string> attackers;
  std::vector<std::pair<std::string, std::string>> attacker_entries;
  for (const pb::Unit& u : snapshot.base().nearby().units()) {
    if (!u.dead() && u.targeting_me()) {
      const std::string key = AttackerKey(u);
      if (attackers.insert(key).second) {
        attacker_entries.push_back({key, std::string(u.name())});
      }
    }
  }
  for (const pb::Unit& u : snapshot.ext().combat().attackers()) {
    if (!u.dead()) {
      const std::string key = AttackerKey(u);
      if (attackers.insert(key).second) {
        attacker_entries.push_back({key, std::string(u.name())});
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
                           absl::StrCat("lost control (", item.label, ")")});
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
          Consider(
              &chosen,
              {Urgency::kUrgent,
               name.empty() ? "attacked" : absl::StrCat("attacked by ", name)});
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
                Consider(&chosen,
                         {Urgency::kUrgent, absl::StrCat("hp<", watch.value)});
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
                          absl::StrCat("mana<", watch.value)});
              }
            } else {
              watch_fired_[i] = false;
            }
          }
          break;
        case Watch::Kind::kEnemyPlayerWithin: {
          std::set<std::string> current_within;
          for (const pb::Unit& u : snapshot.base().nearby().units()) {
            const float dist = u.rel().dist_yd();
            if (!u.dead() && u.kind() == pb::Unit::PLAYER &&
                u.reaction() == pb::Unit::HOSTILE && std::isfinite(dist) &&
                dist <= static_cast<float>(watch.value)) {
              const std::string key = AttackerKey(u);
              current_within.insert(key);
              if (watch_seen_players_[i].count(key) == 0) {
                Consider(&chosen,
                         {Urgency::kUrgent,
                          absl::StrCat("enemy player ", u.name(), " within ",
                                       watch.value, " yd")});
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

  for (const pb::Event& ev : snapshot.events()) {
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
