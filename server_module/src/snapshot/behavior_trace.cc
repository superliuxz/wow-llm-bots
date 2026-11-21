// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/behavior_trace.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <ios>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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

void BehaviorTraceWriter::UnlinkSampleNode(RoleSampleTracker& tracker,
                                           size_t idx,
                                           const SampleListEnds& list) {
  CharSampleNode& node = tracker.nodes[idx];
  const size_t prev = node.*(list.prev);
  const size_t next = node.*(list.next);
  if (prev != kNoSampleNode) {
    tracker.nodes[prev].*(list.next) = next;
  } else if (tracker.*(list.head) == idx) {
    tracker.*(list.head) = next;
  }
  if (next != kNoSampleNode) {
    tracker.nodes[next].*(list.prev) = prev;
  } else if (tracker.*(list.tail) == idx) {
    tracker.*(list.tail) = prev;
  }
  node.*(list.prev) = kNoSampleNode;
  node.*(list.next) = kNoSampleNode;
}

void BehaviorTraceWriter::LinkSampleNodeSorted(RoleSampleTracker& tracker,
                                               size_t idx,
                                               const SampleListEnds& list) {
  const int64_t t = tracker.nodes[idx].last_time_ms;
  size_t after = tracker.*(list.tail);
  while (after != kNoSampleNode && tracker.nodes[after].last_time_ms > t) {
    after = tracker.nodes[after].*(list.prev);
  }
  if (after == kNoSampleNode) {
    const size_t head = tracker.*(list.head);
    tracker.nodes[idx].*(list.prev) = kNoSampleNode;
    tracker.nodes[idx].*(list.next) = head;
    if (head != kNoSampleNode) {
      tracker.nodes[head].*(list.prev) = idx;
    } else {
      tracker.*(list.tail) = idx;
    }
    tracker.*(list.head) = idx;
    return;
  }
  const size_t before = tracker.nodes[after].*(list.next);
  tracker.nodes[idx].*(list.prev) = after;
  tracker.nodes[idx].*(list.next) = before;
  tracker.nodes[after].*(list.next) = idx;
  if (before != kNoSampleNode) {
    tracker.nodes[before].*(list.prev) = idx;
  } else {
    tracker.*(list.tail) = idx;
  }
}

void BehaviorTraceWriter::UpdateSampleNodeOrder(RoleSampleTracker& tracker,
                                                size_t idx,
                                                const SampleListEnds& list) {
  const CharSampleNode& node = tracker.nodes[idx];
  const size_t prev = node.*(list.prev);
  const size_t next = node.*(list.next);
  const bool ok_prev = prev == kNoSampleNode ||
                       tracker.nodes[prev].last_time_ms <= node.last_time_ms;
  const bool ok_next = next == kNoSampleNode ||
                       tracker.nodes[next].last_time_ms >= node.last_time_ms;
  if (ok_prev && ok_next) {
    return;
  }
  UnlinkSampleNode(tracker, idx, list);
  LinkSampleNodeSorted(tracker, idx, list);
}

void BehaviorTraceWriter::EvictSampleNode(RoleSampleTracker& tracker,
                                          size_t idx) {
  CharSampleNode& node = tracker.nodes[idx];
  if (node.in_idle) {
    UnlinkSampleNode(tracker, idx, IdleSampleList());
    node.in_idle = false;
  }
  UnlinkSampleNode(tracker, idx, AllSampleList());
  tracker.index_by_name.erase(node.name);
  node.name.clear();
  std::vector<int64_t>().swap(node.dts);
  node.last_time_ms = 0;
  tracker.free_indices.push_back(idx);
}

TraceSample TraceSampleFromSnapshot(const pb::Snapshot& snapshot,
                                    std::string_view role,
                                    uint32_t map_id,
                                    bool moving,
                                    bool jumping,
                                    bool in_town,
                                    bool in_group,
                                    bool in_dungeon) {
  TraceSample out;
  out.time_ms = snapshot.time_ms;
  out.monotonic_us = snapshot.monotonic_us;
  out.role = std::string(role);
  out.name = snapshot.bot;
  out.map_id = map_id;
  out.zone = snapshot.base.where.zone;
  out.in_town = in_town;
  out.in_group = in_group;
  out.in_dungeon = in_dungeon;
  out.x = snapshot.base.where.position.x;
  out.y = snapshot.base.where.position.y;
  out.z = snapshot.base.where.position.z;
  out.orientation = snapshot.base.where.facing;
  out.moving = moving;
  out.jumping = jumping;
  out.mounted = snapshot.base.self.mounted;
  out.swimming = snapshot.base.self.swimming;
  out.on_taxi = snapshot.base.self.in_flight;
  out.in_combat = snapshot.base.self.in_combat;
  out.dead = snapshot.base.self.state == pb::Self::DEAD_BODY ||
             snapshot.base.self.state == pb::Self::GHOST;
  out.bag_free_slots = snapshot.base.self.indicators.bag_free_slots.value_or(0);
  if (snapshot.ext.pet && snapshot.ext.pet->pet &&
      !snapshot.ext.pet->pet->dead && snapshot.ext.pet->pet->health_pct) {
    out.pet_hp_pct = *snapshot.ext.pet->pet->health_pct;
  }
  out.level = snapshot.base.self.unit.level.value_or(0);
  out.xp = snapshot.base.self.xp;
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
    out.target_hostile =
        t.reaction == pb::Unit::HOSTILE || t.reaction == pb::Unit::NEUTRAL;
    if (out.target_hostile) {
      out.target_dist = t.rel.dist_yd;
      out.target_hp_pct = t.health_pct.value_or(-1.0f);
      out.target_level = t.level.value_or(0);
      if (t.activity.cast && t.activity.cast->spell) {
        out.target_casting = t.activity.cast->spell->name;
      }
      out.target_moving = t.activity.state == pb::Activity::MOVING;
      out.target_targeting_me = t.targeting_me;
      out.target_in_combat =
          t.activity.state == pb::Activity::FIGHTING || t.targeting_me;
    }
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
       ",\"inst\":",
       base::StringPrintf("%u", s.instance_id),
       ",\"zone\":",
       JsonEscape(s.zone),
       ",\"in_town\":",
       s.in_town ? "true" : "false",
       ",\"in_group\":",
       s.in_group ? "true" : "false",
       ",\"in_dungeon\":",
       s.in_dungeon ? "true" : "false",
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
       ",\"mounted\":",
       s.mounted ? "true" : "false",
       ",\"swimming\":",
       s.swimming ? "true" : "false",
       ",\"sitting\":",
       s.sitting ? "true" : "false",
       ",\"on_taxi\":",
       s.on_taxi ? "true" : "false",
       ",\"in_combat\":",
       s.in_combat ? "true" : "false",
       ",\"dead\":",
       s.dead ? "true" : "false",
       ",\"cc_mask\":",
       base::StringPrintf("%u", s.cc_mask),
       ",\"bag_free_slots\":",
       base::StringPrintf("%u", s.bag_free_slots),
       ",\"pet_hp_pct\":",
       base::StringPrintf("%.1f", s.pet_hp_pct),
       ",\"level\":",
       base::StringPrintf("%u", s.level),
       ",\"xp\":",
       base::StringPrintf("%u", s.xp),
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
       ",\"target_moving\":",
       s.target_moving ? "true" : "false",
       ",\"target_hostile\":",
       s.target_hostile ? "true" : "false",
       ",\"target_targeting_me\":",
       s.target_targeting_me ? "true" : "false",
       ",\"target_in_combat\":",
       s.target_in_combat ? "true" : "false",
       ",\"attackers\":",
       base::StringPrintf("%d", s.attackers),
       "}"});
}

std::string FormatTraceChat(const TraceChat& c) {
  const size_t len = c.text_len > 0 ? c.text_len : c.text.size();
  return base::StrCat(
      {"{\"kind\":\"chat\",\"t_ms\":",
       base::StringPrintf("%lld", static_cast<long long>(c.time_ms)),
       ",\"mono_us\":",
       base::StringPrintf("%lld", static_cast<long long>(c.monotonic_us)),
       ",\"role\":", JsonEscape(c.role), ",\"name\":", JsonEscape(c.name),
       ",\"dir\":", JsonEscape(c.direction),
       ",\"channel\":", JsonEscape(c.channel), ",\"peer\":", JsonEscape(c.peer),
       ",\"text\":", JsonEscape(c.text),
       ",\"text_len\":", base::StringPrintf("%zu", len), "}"});
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
                                         size_t max_rotated_files,
                                         size_t max_tracked_chars_per_role)
    : root_dir_(std::move(root_dir)),
      max_file_bytes_(max_file_bytes),
      max_rotated_files_(max_rotated_files),
      max_tracked_chars_per_role_(
          max_tracked_chars_per_role > 0 ? max_tracked_chars_per_role : 1) {
  if (enabled()) {
    worker_ = std::thread(&BehaviorTraceWriter::WriterLoop, this);
  }
}

BehaviorTraceWriter::~BehaviorTraceWriter() {
  if (!worker_.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_one();
  worker_.join();
}

bool BehaviorTraceWriter::WriteSample(const TraceSample& sample) {
  if (!enabled()) {
    return false;
  }
  return Enqueue(sample.role, sample.name, sample.time_ms,
                 FormatTraceSample(sample), /*is_sample=*/true);
}

bool BehaviorTraceWriter::WriteChat(const TraceChat& chat) {
  if (!enabled()) {
    return false;
  }
  return Enqueue(chat.role, chat.name, chat.time_ms, FormatTraceChat(chat));
}

bool BehaviorTraceWriter::WriteEvent(const TraceEvent& event) {
  if (!enabled()) {
    return false;
  }
  return Enqueue(event.role, event.name, event.time_ms,
                 FormatTraceEvent(event));
}

void BehaviorTraceWriter::Flush() {
  if (!worker_.joinable()) {
    return;
  }
  std::unique_lock<std::mutex> lock(mu_);
  flushed_cv_.wait(lock, [this] { return queue_.empty() && !writing_; });
}

uint64_t BehaviorTraceWriter::dropped_lines() const {
  std::lock_guard<std::mutex> lock(mu_);
  return dropped_lines_;
}

uint64_t BehaviorTraceWriter::TakeUntrackedSampleChars() {
  std::lock_guard<std::mutex> lock(mu_);
  return std::exchange(untracked_sample_chars_, 0);
}

std::optional<double> BehaviorTraceWriter::TakeMeasuredSampleHz(
    std::string_view role,
    size_t min_intervals) {
  if (min_intervals == 0) {
    min_intervals = 1;
  }
  std::vector<std::vector<int64_t>> ready_chars;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto role_it = sample_windows_by_role_.find(std::string(role));
    if (role_it == sample_windows_by_role_.end()) {
      return std::nullopt;
    }
    RoleSampleTracker& tracker = role_it->second;
    if (tracker.all_tail == kNoSampleNode) {
      return std::nullopt;
    }
    const int64_t newest_time_ms = tracker.nodes[tracker.all_tail].last_time_ms;
    for (size_t idx = tracker.all_head; idx != kNoSampleNode;) {
      const size_t next = tracker.nodes[idx].all_next;
      CharSampleNode& node = tracker.nodes[idx];
      if (node.dts.size() >= min_intervals) {
        ready_chars.emplace_back();
        ready_chars.back().swap(node.dts);
        if (!node.in_idle) {
          node.in_idle = true;
          LinkSampleNodeSorted(tracker, idx, IdleSampleList());
        }
      } else if (node.dts.empty() && newest_time_ms > node.last_time_ms &&
                 newest_time_ms - node.last_time_ms > kMaxSampleGapMs) {
        EvictSampleNode(tracker, idx);
      }
      idx = next;
    }
  }
  if (ready_chars.empty()) {
    return std::nullopt;
  }
  std::vector<double> char_rates;
  char_rates.reserve(ready_chars.size());
  for (std::vector<int64_t>& dts : ready_chars) {
    std::sort(dts.begin(), dts.end());
    const size_t n = dts.size();
    const double med_ms =
        (n % 2 == 1) ? static_cast<double>(dts[n / 2])
                     : 0.5 * static_cast<double>(dts[n / 2 - 1] + dts[n / 2]);
    if (med_ms > 0.0) {
      char_rates.push_back(std::round((1000.0 / med_ms) * 100.0) / 100.0);
    }
  }
  if (char_rates.empty()) {
    return std::nullopt;
  }
  std::sort(char_rates.begin(), char_rates.end());
  const size_t m = char_rates.size();
  const double role_hz =
      (m % 2 == 1) ? char_rates[m / 2]
                   : 0.5 * (char_rates[m / 2 - 1] + char_rates[m / 2]);
  return std::round(role_hz * 100.0) / 100.0;
}

bool BehaviorTraceWriter::Enqueue(std::string_view role,
                                  std::string_view name,
                                  int64_t time_ms,
                                  std::string line,
                                  bool is_sample) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stop_ || queue_.size() >= kMaxQueueLines) {
      ++dropped_lines_;
      return false;
    }
    if (is_sample) {
      RoleSampleTracker& tracker = sample_windows_by_role_[std::string(role)];
      auto it = tracker.index_by_name.find(std::string(name));
      if (it == tracker.index_by_name.end()) {
        // Evict one expired drained/idle or stale character via the sorted
        // list heads so cap overflow does not scan the character map under
        // `mu_`.
        if (tracker.index_by_name.size() >= max_tracked_chars_per_role_) {
          if (tracker.idle_head != kNoSampleNode &&
              time_ms > tracker.nodes[tracker.idle_head].last_time_ms &&
              time_ms - tracker.nodes[tracker.idle_head].last_time_ms >
                  kMaxSampleGapMs) {
            EvictSampleNode(tracker, tracker.idle_head);
          } else if (tracker.all_head != kNoSampleNode &&
                     time_ms > tracker.nodes[tracker.all_head].last_time_ms &&
                     time_ms - tracker.nodes[tracker.all_head].last_time_ms >
                         kStaleTrackedCharMs) {
            EvictSampleNode(tracker, tracker.all_head);
          }
        }
        if (tracker.index_by_name.size() < max_tracked_chars_per_role_) {
          size_t idx = kNoSampleNode;
          if (!tracker.free_indices.empty()) {
            idx = tracker.free_indices.back();
            tracker.free_indices.pop_back();
          } else {
            idx = tracker.nodes.size();
            tracker.nodes.emplace_back();
          }
          CharSampleNode& node = tracker.nodes[idx];
          node.name = std::string(name);
          node.last_time_ms = time_ms;
          node.dts.clear();
          node.all_prev = kNoSampleNode;
          node.all_next = kNoSampleNode;
          node.idle_prev = kNoSampleNode;
          node.idle_next = kNoSampleNode;
          node.in_idle = true;
          tracker.index_by_name.emplace(node.name, idx);
          LinkSampleNodeSorted(tracker, idx, AllSampleList());
          LinkSampleNodeSorted(tracker, idx, IdleSampleList());
        } else {
          ++untracked_sample_chars_;
        }
      } else {
        const size_t idx = it->second;
        CharSampleNode& node = tracker.nodes[idx];
        const int64_t dt = time_ms - node.last_time_ms;
        if (dt > 0 && dt <= kMaxSampleGapMs) {
          if (node.dts.size() < kMaxRecordedSampleDtsPerChar) {
            node.dts.push_back(dt);
          }
          node.last_time_ms = time_ms;
          if (node.in_idle && !node.dts.empty()) {
            UnlinkSampleNode(tracker, idx, IdleSampleList());
            node.in_idle = false;
          }
          UpdateSampleNodeOrder(tracker, idx, AllSampleList());
        } else if (dt > kMaxSampleGapMs || dt < 0) {
          node.last_time_ms = time_ms;
          UpdateSampleNodeOrder(tracker, idx, AllSampleList());
          if (node.in_idle) {
            UpdateSampleNodeOrder(tracker, idx, IdleSampleList());
          }
        }
      }
    }
    queue_.push_back(
        {std::string(role), std::string(name), time_ms, std::move(line)});
  }
  cv_.notify_one();
  return true;
}

void BehaviorTraceWriter::WriterLoop() {
  for (;;) {
    std::deque<PendingLine> batch;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (queue_.empty() && stop_) {
        flushed_cv_.notify_all();
        return;
      }
      batch.swap(queue_);
      writing_ = true;
    }
    WriteBatch(batch);
    {
      std::lock_guard<std::mutex> lock(mu_);
      writing_ = false;
      if (queue_.empty()) {
        flushed_cv_.notify_all();
      }
    }
  }
}

void BehaviorTraceWriter::WriteBatch(
    const std::deque<PendingLine>& batch) const {
  // Group lines by target file so each file is opened at most once per batch.
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
        // Shift .(N-1).jsonl -> .N.jsonl, ..., .1.jsonl -> .2.jsonl, current
        // -> .1.jsonl so up to `max_rotated_files_` backups survive per day
        // (the rename onto .N.jsonl replaces the oldest archive). With 0
        // archives the current file is truncated in place.
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
        // Archives left past the cap (e.g. the cap was lowered) are removed so
        // the per-day volume bound holds.
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
