// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/behavior_trace.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/stringprintf.h"
#include "snapshot/snapshot.h"

namespace llm_brain {
namespace {

namespace pb = wlb::snapshot;

std::string JsonEscape(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 2);
  out.push_back('"');
  for (unsigned char c : in) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += base::StringPrintf("\\u%04x", c);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

std::string SafePathComponent(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (unsigned char c : raw) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('_');
    }
  }
  return out.empty() ? "unknown" : out;
}

std::string UtcDateString(int64_t time_ms) {
  const std::time_t secs = static_cast<std::time_t>(time_ms / 1000);
  std::tm utc{};
  gmtime_r(&secs, &utc);
  return base::StringPrintf("%04d-%02d-%02d", utc.tm_year + 1900,
                            utc.tm_mon + 1, utc.tm_mday);
}

}  // namespace

TraceSample TraceSampleFromSnapshot(const pb::Snapshot& snapshot,
                                    std::string_view role,
                                    uint32_t map_id,
                                    bool moving,
                                    bool jumping,
                                    bool in_town) {
  TraceSample out;
  out.time_ms = snapshot.time_ms;
  out.monotonic_us = snapshot.monotonic_us;
  out.role = std::string(role);
  out.name = snapshot.bot;
  out.map_id = map_id;
  out.zone = snapshot.base.where.zone;
  out.in_town = in_town;
  out.x = snapshot.base.where.position.x;
  out.y = snapshot.base.where.position.y;
  out.z = snapshot.base.where.position.z;
  out.orientation = snapshot.base.where.facing;
  out.moving = moving;
  out.jumping = jumping;
  out.in_combat = snapshot.base.self.in_combat;
  out.dead = snapshot.base.self.state == pb::Self::DEAD_BODY ||
             snapshot.base.self.state == pb::Self::GHOST;
  if (snapshot.base.self.health_max > 0) {
    out.hp_pct = 100.0f * static_cast<float>(snapshot.base.self.health) /
                 static_cast<float>(snapshot.base.self.health_max);
  } else if (snapshot.base.self.unit.health_pct) {
    out.hp_pct = *snapshot.base.self.unit.health_pct;
  }
  if (snapshot.base.self.power_max > 0) {
    out.mana_pct = 100.0f * static_cast<float>(snapshot.base.self.power) /
                   static_cast<float>(snapshot.base.self.power_max);
  }
  if (snapshot.base.self.casting && snapshot.base.self.casting->spell) {
    out.casting = snapshot.base.self.casting->spell->name;
  }
  if (snapshot.ext.target && snapshot.ext.target->target) {
    out.target = snapshot.ext.target->target->name;
  }
  if (snapshot.ext.combat) {
    out.attackers = static_cast<int>(snapshot.ext.combat->attackers.size());
  }
  return out;
}

std::string FormatTraceSample(const TraceSample& s) {
  return base::StrCat(
      {"{\"kind\":\"sample\",\"t_ms\":",
       base::StringPrintf("%lld", static_cast<long long>(s.time_ms)),
       ",\"mono_us\":",
       base::StringPrintf("%lld", static_cast<long long>(s.monotonic_us)),
       ",\"role\":",
       JsonEscape(s.role),
       ",\"name\":",
       JsonEscape(s.name),
       ",\"map\":",
       base::StringPrintf("%u", s.map_id),
       ",\"zone\":",
       JsonEscape(s.zone),
       ",\"in_town\":",
       s.in_town ? "true" : "false",
       ",\"x\":",
       base::StringPrintf("%.2f", s.x),
       ",\"y\":",
       base::StringPrintf("%.2f", s.y),
       ",\"z\":",
       base::StringPrintf("%.2f", s.z),
       ",\"o\":",
       base::StringPrintf("%.3f", s.orientation),
       ",\"moving\":",
       s.moving ? "true" : "false",
       ",\"jumping\":",
       s.jumping ? "true" : "false",
       ",\"in_combat\":",
       s.in_combat ? "true" : "false",
       ",\"dead\":",
       s.dead ? "true" : "false",
       ",\"hp_pct\":",
       base::StringPrintf("%.1f", s.hp_pct),
       ",\"mana_pct\":",
       base::StringPrintf("%.1f", s.mana_pct),
       ",\"casting\":",
       JsonEscape(s.casting),
       ",\"target\":",
       JsonEscape(s.target),
       ",\"attackers\":",
       base::StringPrintf("%d", s.attackers),
       "}"});
}

std::string FormatTraceChat(const TraceChat& c) {
  return base::StrCat(
      {"{\"kind\":\"chat\",\"t_ms\":",
       base::StringPrintf("%lld", static_cast<long long>(c.time_ms)),
       ",\"mono_us\":",
       base::StringPrintf("%lld", static_cast<long long>(c.monotonic_us)),
       ",\"role\":", JsonEscape(c.role), ",\"name\":", JsonEscape(c.name),
       ",\"dir\":", JsonEscape(c.direction),
       ",\"channel\":", JsonEscape(c.channel), ",\"peer\":", JsonEscape(c.peer),
       ",\"text\":", JsonEscape(c.text), "}"});
}

BehaviorTraceWriter::BehaviorTraceWriter(std::string root_dir)
    : root_dir_(std::move(root_dir)) {}

bool BehaviorTraceWriter::WriteSample(const TraceSample& sample) const {
  if (!enabled()) {
    return false;
  }
  return AppendLine(sample.role, sample.name, sample.time_ms,
                    FormatTraceSample(sample));
}

bool BehaviorTraceWriter::WriteChat(const TraceChat& chat) const {
  if (!enabled()) {
    return false;
  }
  return AppendLine(chat.role, chat.name, chat.time_ms, FormatTraceChat(chat));
}

bool BehaviorTraceWriter::AppendLine(std::string_view role,
                                     std::string_view name,
                                     int64_t time_ms,
                                     std::string_view line) const {
  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::path(root_dir_) /
                                    SafePathComponent(role) /
                                    SafePathComponent(name);
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return false;
  }
  const std::filesystem::path file = dir / (UtcDateString(time_ms) + ".jsonl");
  std::ofstream out(file, std::ios::app);
  if (!out) {
    return false;
  }
  out << line << '\n';
  return static_cast<bool>(out);
}

}  // namespace llm_brain
