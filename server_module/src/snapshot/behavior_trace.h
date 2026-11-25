// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Combat and soak behavior trace logger (C++ cleanup #5). Writes JSONL samples
// and combat/soak events (`damage_dealt`, `damage_taken`, `heal_done`,
// `spell_cast`, `combat_start`, `combat_end`, `mob_kill`, `died`, `xp_gain`,
// `quest_turnin`, `act_sent`, `option_chosen`) read by `boss_meter`,
// `dodge_timeline`, `stall_check`, `soak_scorer`, and `mine_traces`.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_BEHAVIOR_TRACE_H_
#define MOD_LLM_BRAIN_SNAPSHOT_BEHAVIOR_TRACE_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/threading/platform_thread.h"
#include "snapshot/snapshot.h"

namespace llm_brain {

struct TraceSample {
  int64_t time_ms = 0;
  int64_t monotonic_us = 0;
  std::string role;  // "human" or "bot"
  std::string name;
  uint32_t map_id = 0;
  uint32_t instance_id = 0;
  std::string zone;
  bool in_dungeon = false;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  bool moving = false;
  bool in_combat = false;
  bool dead = false;
  float pet_hp_pct = -1.0f;
  uint32_t level = 0;
  uint32_t xp = 0;
  uint32_t xp_max = 0;  // The level's XP bar (soak XP across level-ups).
  float hp_pct = 0.0f;
  float mana_pct = 0.0f;
  std::string casting;
  std::string target;
  float target_dist = -1.0f;
  float target_x = 0.0f;
  float target_y = 0.0f;
  float target_hp_pct = -1.0f;
  uint32_t target_level = 0;
  std::string target_casting;
  float target_cast_radius = -1.0f;
  bool target_targeting_me = false;
};

struct TraceEvent {
  int64_t time_ms = 0;
  int64_t monotonic_us = 0;
  std::string role;  // "human" or "bot"
  std::string name;
  std::string event;   // Combat/soak event name.
  std::string detail;  // Optional event detail (spell/mob/quest name).
  int64_t value = 0;   // Optional numeric payload (damage/heal/xp/quest id).
};

// Extracts a `TraceSample` from a built `Snapshot`.
TraceSample TraceSampleFromSnapshot(const wlb::snapshot::Snapshot& snapshot,
                                    std::string_view role,
                                    uint32_t map_id,
                                    bool moving,
                                    bool in_dungeon = false);

// Serializes a sample or event to a single JSONL line.
std::string FormatTraceSample(const TraceSample& sample);
std::string FormatTraceEvent(const TraceEvent& event);

// Appends JSONL traces under `<root>/<role>/<name>/<YYYY-MM-DD>.jsonl` on a
// background writer thread with size-capped file rotation.
class BehaviorTraceWriter {
 public:
  static constexpr uint64_t kDefaultMaxFileBytes = 64ULL * 1024 * 1024;
  static constexpr size_t kDefaultMaxRotatedFiles = 8;
  static constexpr size_t kMaxQueueLines = 8192;

  explicit BehaviorTraceWriter(
      std::string root_dir,
      uint64_t max_file_bytes = kDefaultMaxFileBytes,
      size_t max_rotated_files = kDefaultMaxRotatedFiles);
  ~BehaviorTraceWriter();

  BehaviorTraceWriter(const BehaviorTraceWriter&) = delete;
  BehaviorTraceWriter& operator=(const BehaviorTraceWriter&) = delete;

  bool enabled() const { return !root_dir_.empty(); }

  bool WriteSample(const TraceSample& sample);
  bool WriteEvent(const TraceEvent& event);

  // Blocks until all currently queued lines have been written to disk.
  void Flush();

  uint64_t dropped_lines() const;

 private:
  struct PendingLine {
    std::string role;
    std::string name;
    int64_t time_ms = 0;
    std::string line;
  };

  bool Enqueue(std::string_view role,
               std::string_view name,
               int64_t time_ms,
               std::string line);
  void WriterLoop();
  void WriteBatch(const std::deque<PendingLine>& batch) const;

  std::string root_dir_;
  uint64_t max_file_bytes_ = kDefaultMaxFileBytes;
  size_t max_rotated_files_ = kDefaultMaxRotatedFiles;
  mutable base::Lock mu_;
  base::ConditionVariable cv_{&mu_};
  base::ConditionVariable flushed_cv_{&mu_};
  std::deque<PendingLine> queue_ GUARDED_BY(mu_);
  uint64_t dropped_lines_ GUARDED_BY(mu_) = 0;
  bool writing_ GUARDED_BY(mu_) = false;
  bool stop_ GUARDED_BY(mu_) = false;
  base::PlatformThreadHandle worker_;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_BEHAVIOR_TRACE_H_
