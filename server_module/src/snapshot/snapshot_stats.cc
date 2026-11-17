// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/snapshot_stats.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "base/time/time.h"

namespace llm_brain {
namespace {

// The value at quantile `q` (0..1) of sorted `values` (nearest rank).
int64_t Quantile(const std::vector<int64_t>& sorted, double q) {
  const size_t rank = static_cast<size_t>(q * (sorted.size() - 1) + 0.5);
  return sorted[rank];
}

std::string AvgMax(const char* name, const std::vector<size_t>& values) {
  size_t sum = 0;
  size_t max = 0;
  for (size_t value : values) {
    sum += value;
    max = std::max(max, value);
  }
  return std::string(" ") + name +
         " avg=" + std::to_string(sum / values.size()) +
         " max=" + std::to_string(max);
}

}  // namespace

SnapshotStats::SnapshotStats() = default;
SnapshotStats::~SnapshotStats() = default;

void SnapshotStats::Add(const Sample& sample) {
  samples_.push_back(sample);
}

std::string SnapshotStats::Summary() const {
  if (samples_.empty()) {
    return "n=0";
  }
  std::vector<int64_t> times_us;
  std::vector<size_t> bytes;
  std::vector<size_t> units;
  std::vector<size_t> objects;
  std::vector<size_t> hidden;
  std::vector<size_t> raycasts;
  int64_t total_us = 0;
  for (const Sample& sample : samples_) {
    times_us.push_back(sample.build_time.InMicroseconds());
    total_us += times_us.back();
    bytes.push_back(sample.bytes);
    units.push_back(sample.units);
    objects.push_back(sample.objects);
    hidden.push_back(sample.hidden);
    raycasts.push_back(sample.raycasts);
  }
  std::sort(times_us.begin(), times_us.end());
  return "n=" + std::to_string(samples_.size()) +
         " build p50=" + std::to_string(Quantile(times_us, 0.5)) +
         "us p99=" + std::to_string(Quantile(times_us, 0.99)) +
         "us max=" + std::to_string(times_us.back()) +
         "us total=" + std::to_string(total_us / 1000) + "ms" +
         AvgMax("bytes", bytes) + AvgMax("units", units) +
         AvgMax("objects", objects) + AvgMax("hidden", hidden) +
         AvgMax("raycasts", raycasts);
}

void SnapshotStats::Reset() {
  samples_.clear();
}

}  // namespace llm_brain
