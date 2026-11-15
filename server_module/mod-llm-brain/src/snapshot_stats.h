// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_STATS_H_
#define MOD_LLM_BRAIN_SNAPSHOT_STATS_H_

#include <cstddef>
#include <string>
#include <vector>

#include "base/time/time.h"

namespace llm_brain {

// Aggregates the cost of building snapshots over a reporting window (the
// module logs a summary once a minute), so a busy area slowing the world
// thread shows up in the server log.
class SnapshotStats {
 public:
  struct Sample {
    base::TimeDelta build_time;
    size_t bytes = 0;
    size_t units = 0;
    size_t objects = 0;
    // Far and out of line of sight (or not yet checked): left out.
    size_t hidden = 0;
    size_t raycasts = 0;
  };

  SnapshotStats();
  SnapshotStats(const SnapshotStats&) = delete;
  SnapshotStats& operator=(const SnapshotStats&) = delete;
  ~SnapshotStats();

  void Add(const Sample& sample);
  size_t count() const { return samples_.size(); }

  // "n=120 build p50=45us p99=210us max=380us total=6ms bytes avg=950
  // max=1210 units avg=7 max=9 objects avg=2 max=3 hidden avg=4 max=6
  // raycasts avg=3 max=32"; "n=0" when empty.
  std::string Summary() const;
  void Reset();

 private:
  std::vector<Sample> samples_;
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_STATS_H_
