// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Unified behavior trace logger for both human players and LLM bots
// (docs/design.md, "Evals"). Both roles write the exact same JSONL schema so
// `tools/bench/human_metrics.py` can compare movement, reaction timing, chat,
// and pacing distributions directly against a human baseline.

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
  std::string zone;
  bool in_town = false;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float orientation = 0.0f;
  bool moving = false;
  bool jumping = false;
  bool in_combat = false;
  bool dead = false;
  uint32_t level = 0;
  uint32_t xp = 0;
  float hp_pct = 0.0f;
  float mana_pct = 0.0f;
  std::string casting;
  std::string target;
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

// Extracts a `TraceSample` from a built `Snapshot` (used for bots).
TraceSample TraceSampleFromSnapshot(const wlb::snapshot::Snapshot& snapshot,
                                    std::string_view role,
                                    uint32_t map_id,
                                    bool moving,
                                    bool jumping,
                                    bool in_town);

// Serializes a sample or chat event to a single JSONL line (no trailing '\n').
std::string FormatTraceSample(const TraceSample& sample);
std::string FormatTraceChat(const TraceChat& chat);

// Appends unified JSONL traces under `<root>/<role>/<name>/<YYYY-MM-DD>.jsonl`.
// Enqueues formatted lines without disk I/O on the caller (world) thread and
// flushes them on a background writer thread with size-capped file rotation.
class BehaviorTraceWriter {
 public:
  static constexpr uint64_t kDefaultMaxFileBytes = 10ULL * 1024 * 1024;
  static constexpr size_t kMaxQueueLines = 8192;

  explicit BehaviorTraceWriter(std::string root_dir,
                               uint64_t max_file_bytes = kDefaultMaxFileBytes);
  ~BehaviorTraceWriter();

  BehaviorTraceWriter(const BehaviorTraceWriter&) = delete;
  BehaviorTraceWriter& operator=(const BehaviorTraceWriter&) = delete;

  bool enabled() const { return !root_dir_.empty(); }

  bool WriteSample(const TraceSample& sample);
  bool WriteChat(const TraceChat& chat);

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
