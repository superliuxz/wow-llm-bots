// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Unified behavior trace logger for both human players and LLM bots
// (docs/plans/self-improving-loop.md, section F). Both roles write the exact
// same JSONL schema so `tools/bench/trace_parity.py` can reconstruct both into
// one canonical 5 Hz `(pos, facing)` + event timeline.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_BEHAVIOR_TRACE_H_
#define MOD_LLM_BRAIN_SNAPSHOT_BEHAVIOR_TRACE_H_

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

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
  bool in_town = false;
  bool in_group = false;
  bool in_dungeon = false;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float orientation = 0.0f;
  bool moving = false;
  bool jumping = false;
  bool mounted = false;
  bool swimming = false;
  bool sitting = false;
  bool on_taxi = false;
  bool in_combat = false;
  bool dead = false;
  uint32_t cc_mask = 0;
  uint32_t bag_free_slots = 0;
  float pet_hp_pct = -1.0f;
  uint32_t level = 0;
  uint32_t xp = 0;
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
  bool target_moving = false;
  bool target_hostile = false;
  bool target_targeting_me = false;
  bool target_in_combat = false;
  int attackers = 0;
};

struct TraceChat {
  int64_t time_ms = 0;
  int64_t monotonic_us = 0;
  std::string role;       // "human" or "bot"
  std::string name;       // whose trace file this belongs to
  std::string direction;  // "in" or "out"
  std::string channel;    // "whisper", "say", "party", ...
  std::string peer;       // sender (for "in") or recipient (for "out")
  std::string text;       // Empty when redacted (e.g. human whisper privacy).
  size_t text_len = 0;    // Original byte length when `text` is redacted.
};

// Explicit timestamped gameplay event recorded identically for humans and bots
// (docs/plans/self-improving-loop.md, section F).
struct TraceEvent {
  int64_t time_ms = 0;
  int64_t monotonic_us = 0;
  std::string role;  // "human" or "bot"
  std::string name;
  std::string event;   // "damage_taken", "combat_start", "combat_end",
                       // "cast_start", "loot", "quest_accept", "quest_turnin"
  std::string detail;  // Optional event detail (e.g. spell name or item).
  int64_t value = 0;   // Optional numeric payload (e.g. damage, quest id).
};

// Extracts a `TraceSample` from a built `Snapshot` (used for bots).
TraceSample TraceSampleFromSnapshot(const wlb::snapshot::Snapshot& snapshot,
                                    std::string_view role,
                                    uint32_t map_id,
                                    bool moving,
                                    bool jumping,
                                    bool in_town,
                                    bool in_group = false,
                                    bool in_dungeon = false);

// Serializes a sample, chat, or gameplay event to a single JSONL line.
std::string FormatTraceSample(const TraceSample& sample);
std::string FormatTraceChat(const TraceChat& chat);
std::string FormatTraceEvent(const TraceEvent& event);

// Appends unified JSONL traces under `<root>/<role>/<name>/<YYYY-MM-DD>.jsonl`.
// Enqueues formatted lines without disk I/O on the caller (world) thread and
// flushes them on a background writer thread with size-capped file rotation:
// each day file rotates at `max_file_bytes` and keeps `max_rotated_files`
// archives, so one character's day is capped at
// `max_file_bytes * (max_rotated_files + 1)`; the oldest archive is
// overwritten.
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
  bool WriteChat(const TraceChat& chat);
  bool WriteEvent(const TraceEvent& event);

  // Blocks until all currently queued lines have been written to disk.
  void Flush();

  // Number of trace lines dropped because the queue reached `kMaxQueueLines`
  // or the writer was stopping.
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
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::condition_variable flushed_cv_;
  std::deque<PendingLine> queue_;
  uint64_t dropped_lines_ = 0;
  bool writing_ = false;
  bool stop_ = false;
  std::thread worker_;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_BEHAVIOR_TRACE_H_
