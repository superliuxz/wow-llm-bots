// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/behavior_trace.h"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/threading/platform_thread.h"
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
                                    bool in_dungeon) {
  TraceSample out;
  out.time_ms = snapshot.time_ms;
  out.monotonic_us = snapshot.monotonic_us;
  out.role = std::string(role);
  out.name = snapshot.bot;
  out.map_id = map_id;
  out.zone = snapshot.base.where.zone;
  out.in_dungeon = in_dungeon;
  out.x = snapshot.base.where.position.x;
  out.y = snapshot.base.where.position.y;
  out.z = snapshot.base.where.position.z;
  out.moving = moving;
  out.in_combat = snapshot.base.self.in_combat;
  out.dead = snapshot.base.self.state == pb::Self::DEAD_BODY ||
             snapshot.base.self.state == pb::Self::GHOST;
  if (snapshot.ext.pet && snapshot.ext.pet->pet &&
      !snapshot.ext.pet->pet->dead && snapshot.ext.pet->pet->health_pct) {
    out.pet_hp_pct = *snapshot.ext.pet->pet->health_pct;
  }
  out.level = snapshot.base.self.unit.level.value_or(0);
  out.xp = snapshot.base.self.xp;
  out.xp_max = snapshot.base.self.xp_next;
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
    const pb::Unit& t = *snapshot.ext.target->target;
    out.target = t.name;
    const bool hostile =
        t.reaction == pb::Unit::HOSTILE || t.reaction == pb::Unit::NEUTRAL;
    if (hostile) {
      out.target_dist = t.rel.dist_yd;
      out.target_hp_pct = t.health_pct.value_or(-1.0f);
      out.target_level = t.level.value_or(0);
      if (t.activity.cast && t.activity.cast->spell) {
        out.target_casting = t.activity.cast->spell->name;
      }
      out.target_targeting_me = t.targeting_me;
    }
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
       ",\"inst\":",
       base::StringPrintf("%u", s.instance_id),
       ",\"zone\":",
       JsonEscape(s.zone),
       ",\"in_dungeon\":",
       s.in_dungeon ? "true" : "false",
       ",\"x\":",
       base::StringPrintf("%.2f", s.x),
       ",\"y\":",
       base::StringPrintf("%.2f", s.y),
       ",\"z\":",
       base::StringPrintf("%.2f", s.z),
       ",\"moving\":",
       s.moving ? "true" : "false",
       ",\"in_combat\":",
       s.in_combat ? "true" : "false",
       ",\"dead\":",
       s.dead ? "true" : "false",
       ",\"pet_hp_pct\":",
       base::StringPrintf("%.1f", s.pet_hp_pct),
       ",\"level\":",
       base::StringPrintf("%u", s.level),
       ",\"xp\":",
       base::StringPrintf("%u", s.xp),
       ",\"xp_max\":",
       base::StringPrintf("%u", s.xp_max),
       ",\"hp_pct\":",
       base::StringPrintf("%.1f", s.hp_pct),
       ",\"mana_pct\":",
       base::StringPrintf("%.1f", s.mana_pct),
       ",\"casting\":",
       JsonEscape(s.casting),
       ",\"target\":",
       JsonEscape(s.target),
       ",\"target_dist\":",
       base::StringPrintf("%.2f", s.target_dist),
       ",\"target_x\":",
       base::StringPrintf("%.1f", s.target_x),
       ",\"target_y\":",
       base::StringPrintf("%.1f", s.target_y),
       ",\"target_hp_pct\":",
       base::StringPrintf("%.1f", s.target_hp_pct),
       ",\"target_level\":",
       base::StringPrintf("%u", s.target_level),
       ",\"target_casting\":",
       JsonEscape(s.target_casting),
       ",\"target_cast_radius\":",
       base::StringPrintf("%.1f", s.target_cast_radius),
       ",\"target_targeting_me\":",
       s.target_targeting_me ? "true" : "false",
       "}"});
}

std::string FormatTraceEvent(const TraceEvent& e) {
  return base::StrCat(
      {"{\"kind\":\"event\",\"t_ms\":",
       base::StringPrintf("%lld", static_cast<long long>(e.time_ms)),
       ",\"mono_us\":",
       base::StringPrintf("%lld", static_cast<long long>(e.monotonic_us)),
       ",\"role\":", JsonEscape(e.role), ",\"name\":", JsonEscape(e.name),
       ",\"event\":", JsonEscape(e.event), ",\"detail\":", JsonEscape(e.detail),
       ",\"value\":",
       base::StringPrintf("%lld", static_cast<long long>(e.value)), "}"});
}

BehaviorTraceWriter::BehaviorTraceWriter(std::string root_dir,
                                         uint64_t max_file_bytes,
                                         size_t max_rotated_files)
    : root_dir_(std::move(root_dir)),
      max_file_bytes_(max_file_bytes),
      max_rotated_files_(max_rotated_files) {
  if (enabled()) {
    worker_ = base::PlatformThread::Create("behavior-trace",
                                           [this] { WriterLoop(); });
  }
}

BehaviorTraceWriter::~BehaviorTraceWriter() {
  if (worker_.is_null()) {
    return;
  }
  {
    base::AutoLock lock(mu_);
    stop_ = true;
  }
  cv_.Signal();
  base::PlatformThread::Join(worker_);
}

bool BehaviorTraceWriter::WriteSample(const TraceSample& sample) {
  if (!enabled()) {
    return false;
  }
  return Enqueue(sample.role, sample.name, sample.time_ms,
                 FormatTraceSample(sample));
}

bool BehaviorTraceWriter::WriteEvent(const TraceEvent& event) {
  if (!enabled()) {
    return false;
  }
  return Enqueue(event.role, event.name, event.time_ms,
                 FormatTraceEvent(event));
}

void BehaviorTraceWriter::Flush() {
  if (worker_.is_null()) {
    return;
  }
  base::AutoLock lock(mu_);
  while (!queue_.empty() || writing_) {
    flushed_cv_.Wait();
  }
}

uint64_t BehaviorTraceWriter::dropped_lines() const {
  base::AutoLock lock(mu_);
  return dropped_lines_;
}

bool BehaviorTraceWriter::Enqueue(std::string_view role,
                                  std::string_view name,
                                  int64_t time_ms,
                                  std::string line) {
  {
    base::AutoLock lock(mu_);
    if (stop_ || queue_.size() >= kMaxQueueLines) {
      ++dropped_lines_;
      return false;
    }
    queue_.push_back(
        {std::string(role), std::string(name), time_ms, std::move(line)});
  }
  cv_.Signal();
  return true;
}

void BehaviorTraceWriter::WriterLoop() {
  for (;;) {
    std::deque<PendingLine> batch;
    {
      base::AutoLock lock(mu_);
      while (!stop_ && queue_.empty()) {
        cv_.Wait();
      }
      if (queue_.empty() && stop_) {
        flushed_cv_.Broadcast();
        return;
      }
      batch.swap(queue_);
      writing_ = true;
    }
    WriteBatch(batch);
    {
      base::AutoLock lock(mu_);
      writing_ = false;
      if (queue_.empty()) {
        flushed_cv_.Broadcast();
      }
    }
  }
}

void BehaviorTraceWriter::WriteBatch(
    const std::deque<PendingLine>& batch) const {
  std::unordered_map<std::string, std::vector<const PendingLine*>> grouped;
  for (const PendingLine& item : batch) {
    const std::filesystem::path dir = std::filesystem::path(root_dir_) /
                                      SafePathComponent(item.role) /
                                      SafePathComponent(item.name);
    const std::filesystem::path file =
        dir / (UtcDateString(item.time_ms) + ".jsonl");
    grouped[file.string()].push_back(&item);
  }
  for (const auto& [path_str, items] : grouped) {
    const std::filesystem::path file(path_str);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
      continue;
    }
    if (max_file_bytes_ > 0 && std::filesystem::exists(file, ec)) {
      const uint64_t sz = std::filesystem::file_size(file, ec);
      if (!ec && sz >= max_file_bytes_) {
        if (max_rotated_files_ == 0) {
          std::filesystem::resize_file(file, 0, ec);
        } else {
          for (size_t idx = max_rotated_files_; idx >= 2; --idx) {
            std::filesystem::path src = file;
            src.replace_extension(base::StringPrintf(".%zu.jsonl", idx - 1));
            std::filesystem::path dst = file;
            dst.replace_extension(base::StringPrintf(".%zu.jsonl", idx));
            std::error_code rot_ec;
            if (std::filesystem::exists(src, rot_ec)) {
              std::filesystem::rename(src, dst, rot_ec);
            }
          }
          std::filesystem::path rotated = file;
          rotated.replace_extension(".1.jsonl");
          std::filesystem::rename(file, rotated, ec);
        }
        for (size_t idx = max_rotated_files_ + 1;; ++idx) {
          std::filesystem::path stale = file;
          stale.replace_extension(base::StringPrintf(".%zu.jsonl", idx));
          std::error_code stale_ec;
          if (!std::filesystem::remove(stale, stale_ec)) {
            break;
          }
        }
      }
    }
    std::ofstream out(file, std::ios::app);
    if (!out) {
      continue;
    }
    for (const PendingLine* item : items) {
      out << item->line << '\n';
    }
  }
}

}  // namespace llm_brain
