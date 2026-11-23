// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "screen/knowledge.h"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "nlohmann/json.hpp"
#include "runtime/file_util.h"
#include "snapshot/snapshot.h"

namespace harness {
namespace {

constexpr size_t kMaxKnowledgeBytes = 1200;
constexpr size_t kMaxQuestsInPrompt = 3;

std::string OneLineClean(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c == '\n' || c == '\r') {
      c = ' ';
    }
  }
  return out;
}

std::string StringField(const nlohmann::json& obj, const char* key) {
  auto it = obj.find(key);
  if (it != obj.end() && it->is_string()) {
    return OneLineClean(it->get_ref<const std::string&>());
  }
  return {};
}

uint32_t UintField(const nlohmann::json& obj, const char* key) {
  auto it = obj.find(key);
  if (it != obj.end() && it->is_number_integer()) {
    const int64_t v = it->get<int64_t>();
    return v > 0 ? static_cast<uint32_t>(v) : 0u;
  }
  return 0u;
}

std::vector<std::string> StringListField(const nlohmann::json& obj,
                                         const char* key) {
  std::vector<std::string> out;
  auto it = obj.find(key);
  if (it != obj.end() && it->is_array()) {
    for (const auto& item : *it) {
      if (item.is_string()) {
        std::string s = OneLineClean(item.get_ref<const std::string&>());
        if (!s.empty()) {
          out.push_back(std::move(s));
        }
      }
    }
  }
  return out;
}

void AppendLineCapped(std::string* out, std::string_view line) {
  if (line.empty()) {
    return;
  }
  const size_t sep = out->empty() ? 0 : 1;
  if (out->size() + sep + line.size() > kMaxKnowledgeBytes) {
    return;
  }
  if (!out->empty()) {
    out->push_back('\n');
  }
  out->append(line);
}

}  // namespace

std::optional<WorldKnowledge> WorldKnowledge::LoadFile(std::string_view path,
                                                       std::string* error) {
  const std::optional<std::string> raw = ReadFile(std::string(path));
  if (!raw) {
    if (error) {
      *error = base::StrCat({"cannot read knowledge file: ", path});
    }
    return std::nullopt;
  }
  return ParseJson(*raw, error);
}

std::optional<WorldKnowledge> WorldKnowledge::ParseJson(
    std::string_view json_text,
    std::string* error) {
  nlohmann::json root = nlohmann::json::parse(json_text, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    if (error) {
      *error = "invalid world knowledge JSON";
    }
    return std::nullopt;
  }
  WorldKnowledge wk;
  if (auto it = root.find("quests"); it != root.end() && it->is_object()) {
    for (const auto& [k, v] : it->items()) {
      uint32_t qid = 0;
      if (!v.is_object() || !base::StringToUint(k, &qid) || qid == 0) {
        continue;
      }
      QuestInfo qi;
      qi.title = StringField(v, "title");
      qi.level = UintField(v, "level");
      qi.min_level = UintField(v, "min_level");
      qi.log = StringField(v, "log");
      qi.starter = StringField(v, "starter");
      qi.ender = StringField(v, "ender");
      qi.drops = StringListField(v, "drops");
      qi.tip = StringField(v, "tip");
      wk.quests_.emplace(qid, std::move(qi));
    }
  }
  if (auto it = root.find("zones"); it != root.end() && it->is_object()) {
    for (const auto& [k, v] : it->items()) {
      if (!v.is_object() || k.empty()) {
        continue;
      }
      ZoneInfo zi;
      zi.min_level = UintField(v, "min_level");
      zi.max_level = UintField(v, "max_level");
      zi.faction = StringField(v, "faction");
      zi.summary = StringField(v, "summary");
      zi.next_zones = StringListField(v, "next_zones");
      wk.zones_.emplace(k, std::move(zi));
    }
  }
  if (auto it = root.find("dungeons"); it != root.end() && it->is_object()) {
    for (const auto& [k, v] : it->items()) {
      if (!v.is_object() || k.empty()) {
        continue;
      }
      DungeonInfo di;
      di.level = StringField(v, "level");
      di.summary = StringField(v, "summary");
      if (!di.summary.empty()) {
        wk.dungeons_.emplace(k, std::move(di));
      }
    }
  }
  return wk;
}

std::string WorldKnowledge::SelectRelevant(
    const wlb::snapshot::Snapshot& snapshot,
    bool has_boss_notes) const {
  if (empty()) {
    return {};
  }
  std::string out;
  const wlb::snapshot::Where& where = snapshot.base.where;
  const uint32_t bot_level = snapshot.base.self.unit.level.value_or(0);

  // 1) Open-world zone level range + outleveled-zone guidance.
  if (!where.in_instance && !where.zone.empty()) {
    if (auto it = zones_.find(where.zone); it != zones_.end()) {
      const ZoneInfo& zi = it->second;
      if (zi.min_level > 0 && zi.max_level > 0) {
        std::string line =
            base::StrCat({"- Zone: ", where.zone, " (levels ",
                          base::NumberToString(zi.min_level), "-",
                          base::NumberToString(zi.max_level), ")"});
        if (!zi.faction.empty()) {
          base::StrAppend(&line, {", ", zi.faction});
        }
        AppendLineCapped(&out, line);
        if (bot_level > 0 && bot_level > zi.max_level + 2 &&
            !zi.next_zones.empty()) {
          AppendLineCapped(
              &out,
              base::StrCat({"- Outleveled zone: you are level ",
                            base::NumberToString(bot_level), "; next zones: ",
                            base::JoinString(zi.next_zones, ", ")}));
        }
      }
    }
  }

  // 2) Dungeon/instance wiki fallback when no curated boss notes exist.
  if (where.in_instance && !has_boss_notes) {
    const std::string* key = nullptr;
    auto it = dungeons_.find(where.zone);
    if (it != dungeons_.end()) {
      key = &where.zone;
    } else {
      it = dungeons_.find(where.map);
      if (it != dungeons_.end()) {
        key = &where.map;
      }
    }
    if (key != nullptr && it != dungeons_.end()) {
      std::string line = base::StrCat({"- Instance: ", *key});
      if (!it->second.level.empty()) {
        base::StrAppend(&line, {" (levels ", it->second.level, ")"});
      }
      base::StrAppend(&line, {" — ", it->second.summary});
      AppendLineCapped(&out, line);
    }
  }

  // 3) Active quests from quest_tracker (incomplete first, then complete).
  struct TrackedQuest {
    uint32_t quest_id = 0;
    bool complete = false;
  };
  std::vector<TrackedQuest> ordered;
  std::unordered_set<uint32_t> seen_qids;
  for (const wlb::snapshot::Objective& obj : snapshot.ext.quest_tracker) {
    if (!obj.quest || obj.quest->quest_id == 0) {
      continue;
    }
    if (!seen_qids.insert(obj.quest->quest_id).second) {
      continue;
    }
    ordered.push_back({obj.quest->quest_id, obj.quest->complete});
  }
  size_t emitted = 0;
  for (bool want_complete : {false, true}) {
    for (const TrackedQuest& tq : ordered) {
      if (emitted >= kMaxQuestsInPrompt) {
        break;
      }
      if (tq.complete != want_complete) {
        continue;
      }
      auto it = quests_.find(tq.quest_id);
      if (it == quests_.end()) {
        continue;
      }
      const QuestInfo& qi = it->second;
      std::string line =
          base::StrCat({"- Quest \"", qi.title.empty() ? "?" : qi.title, "\""});
      if (qi.level > 0) {
        base::StrAppend(&line, {" (lvl ", base::NumberToString(qi.level), ")"});
      }
      if (tq.complete) {
        if (!qi.ender.empty()) {
          base::StrAppend(&line, {": complete — turn in to ", qi.ender, "."});
        } else {
          base::StrAppend(&line, {": complete."});
        }
      } else {
        if (!qi.log.empty()) {
          base::StrAppend(&line, {": ", qi.log});
        }
        if (!qi.drops.empty()) {
          base::StrAppend(
              &line, {" Drops from: ", base::JoinString(qi.drops, ", "), "."});
        }
        if (!qi.tip.empty()) {
          base::StrAppend(&line, {" Tip: ", qi.tip});
        }
      }
      AppendLineCapped(&out, line);
      ++emitted;
    }
  }
  return out;
}

}  // namespace harness
