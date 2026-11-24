// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "runtime/replay.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "base/functional/callback.h"

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "llm/llm_client.h"
#include "llm/scheduler.h"
#include "plan/plan.h"
#include "plan/triggers.h"
#include "runtime/file_util.h"
#include "screen/prompt.h"
#include "screen/screen_renderer.h"
#include "snapshot/snapshot.h"

namespace harness {
namespace {

namespace fs = std::filesystem;
namespace pb = wlb::snapshot;

std::string_view ShortOptionTag(OptionKind kind) {
  switch (kind) {
    case OptionKind::kAttack:
      return "attack";
    case OptionKind::kTalk:
      return "talk";
    case OptionKind::kLoot:
      return "loot";
    case OptionKind::kGather:
      return "gather";
    case OptionKind::kUse:
      return "use";
    case OptionKind::kKeep:
      return "keep";
    case OptionKind::kExplore:
      return "explore";
    case OptionKind::kRest:
      return "rest";
    case OptionKind::kTown:
      return "town";
    case OptionKind::kInvite:
      return "invite";
    case OptionKind::kAcceptInvite:
      return "accept_invite";
    case OptionKind::kDeclineInvite:
      return "decline_invite";
    case OptionKind::kLeaveGroup:
      return "leave_group";
  }
  return "keep";
}

base::TimeDelta SecondsToDelta(double seconds) {
  return base::Microseconds(static_cast<int64_t>(std::llround(seconds * 1e6)));
}

double Percentile(std::vector<double> values, double q) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const size_t idx =
      std::min(values.size() - 1, static_cast<size_t>(q * values.size()));
  return values[idx];
}

struct PreparedWake {
  double t_s = 0.0;
  std::string bot;
  bool urgent = false;
  LlmRequest request;
  int option_count = 0;
};

struct BotReplayData {
  std::string bot;
  double duration_s = 0.0;
  std::vector<PreparedWake> wakes;
};

bool ParseDouble(std::string_view s, double* out) {
  if (s.empty()) {
    return false;
  }
  try {
    size_t pos = 0;
    const std::string copy(s);
    const double val = std::stod(copy, &pos);
    if (pos != copy.size() || !std::isfinite(val)) {
      return false;
    }
    *out = val;
    return true;
  } catch (...) {
    return false;
  }
}

std::vector<BotReplayData> BuildReplayWakes(const ReplayOptions& options,
                                            const std::string& rules) {
  std::vector<fs::path> bot_dirs;
  std::error_code ec;
  for (const fs::directory_entry& ent :
       fs::directory_iterator(options.snapshot_dir, ec)) {
    if (ent.is_directory()) {
      bot_dirs.push_back(ent.path());
    }
  }
  std::sort(bot_dirs.begin(), bot_dirs.end());

  std::mt19937_64 rng(1);
  std::vector<BotReplayData> out;
  for (const fs::path& dir : bot_dirs) {
    std::vector<std::pair<int64_t, fs::path>> files;
    for (const fs::directory_entry& ent : fs::directory_iterator(dir, ec)) {
      if (!ent.is_regular_file() || ent.path().extension() != ".json") {
        continue;
      }
      int64_t seq = 0;
      if (!base::StringToInt64(ent.path().stem().string(), &seq)) {
        continue;
      }
      files.emplace_back(seq, ent.path());
    }
    std::sort(files.begin(), files.end());
    if (files.size() < 2) {
      continue;
    }

    std::vector<pb::Snapshot> frames;
    frames.reserve(files.size());
    for (const auto& [seq, path] : files) {
      const std::optional<std::string> raw = ReadFile(path.string());
      pb::Snapshot snap;
      std::string err;
      if (raw && pb::ReadJson(*raw, &snap, &err)) {
        frames.push_back(std::move(snap));
      }
    }
    if (frames.size() < 2) {
      continue;
    }

    BotReplayData data;
    data.bot = dir.filename().string();
    const int64_t t0_ms = frames.front().time_ms;
    data.duration_s = std::max(
        1.0, static_cast<double>(frames.back().time_ms - t0_ms) / 1000.0);

    TriggerEvaluator triggers;
    std::exponential_distribution<double> exp_dist(
        options.plan_s > 0.0 ? (1.0 / options.plan_s) : 1e-6);
    double next_plan_end = options.plan_s > 0.0 ? exp_dist(rng) : 1e18;

    for (const pb::Snapshot& snap : frames) {
      const double t_s =
          std::max(0.0, static_cast<double>(snap.time_ms - t0_ms) / 1000.0);
      const std::optional<WakeTrigger> trig = triggers.EvaluateSnapshot(snap);
      bool wake = false;
      bool urgent = false;
      std::string reason;
      if (trig) {
        wake = true;
        urgent = trig->urgency <= Urgency::kP1Danger;
        reason = trig->reason;
      } else if (t_s >= next_plan_end) {
        wake = true;
        reason = "plan finished";
      }
      if (!wake) {
        continue;
      }
      next_plan_end = t_s + (options.plan_s > 0.0 ? exp_dist(rng) : 1e18);
      const Screen screen = RenderScreen(snap);
      PromptInput pi;
      pi.rules = rules;
      pi.bot_name = data.bot;
      pi.wake_reason = reason;
      pi.temperature = options.temperature;
      PreparedWake w;
      w.t_s = t_s;
      w.bot = data.bot;
      w.urgent = urgent;
      w.request = BuildPlanRequest(pi, screen);
      w.option_count = static_cast<int>(screen.options.size());
      data.wakes.push_back(std::move(w));
    }
    if (!data.wakes.empty()) {
      out.push_back(std::move(data));
    }
  }
  return out;
}

}  // namespace

std::optional<ReplayOptions> ParseReplayArgs(
    const std::vector<std::string>& args,
    std::string* error) {
  ReplayOptions opts;
  for (const std::string& arg : args) {
    if (arg.starts_with("--mode=")) {
      opts.mode = arg.substr(7);
      if (opts.mode != "capacity" && opts.mode != "load") {
        *error = "replay: --mode must be capacity or load";
        return std::nullopt;
      }
    } else if (arg.starts_with("--rules=")) {
      opts.rules_path = arg.substr(8);
    } else if (arg.starts_with("--host=")) {
      opts.endpoint.host = arg.substr(7);
    } else if (arg.starts_with("--port=")) {
      if (!base::StringToInt(arg.substr(7), &opts.endpoint.port) ||
          opts.endpoint.port <= 0) {
        *error = "replay: bad --port";
        return std::nullopt;
      }
    } else if (arg.starts_with("--bots=")) {
      if (!base::StringToInt(arg.substr(7), &opts.bots) || opts.bots <= 0) {
        *error = "replay: bad --bots";
        return std::nullopt;
      }
    } else if (arg.starts_with("--slots=")) {
      if (!base::StringToInt(arg.substr(8), &opts.slots) || opts.slots <= 0) {
        *error = "replay: bad --slots";
        return std::nullopt;
      }
    } else if (arg.starts_with("--plan-s=")) {
      if (!ParseDouble(arg.substr(9), &opts.plan_s) || opts.plan_s < 0.0) {
        *error = "replay: bad --plan-s";
        return std::nullopt;
      }
    } else if (arg.starts_with("--seconds=")) {
      if (!ParseDouble(arg.substr(10), &opts.seconds) || opts.seconds <= 0.0) {
        *error = "replay: bad --seconds";
        return std::nullopt;
      }
    } else if (arg.starts_with("--warmup=")) {
      if (!ParseDouble(arg.substr(9), &opts.warmup_s) || opts.warmup_s < 0.0) {
        *error = "replay: bad --warmup";
        return std::nullopt;
      }
    } else if (arg.starts_with("--temperature=")) {
      double t = 0.0;
      if (!ParseDouble(arg.substr(14), &t) || t < 0.0) {
        *error = "replay: bad --temperature";
        return std::nullopt;
      }
      opts.temperature = static_cast<float>(t);
    } else if (arg.starts_with("--")) {
      *error = "replay: unknown flag " + arg;
      return std::nullopt;
    } else if (opts.snapshot_dir.empty()) {
      opts.snapshot_dir = arg;
    } else {
      *error = "replay: unexpected argument " + arg;
      return std::nullopt;
    }
  }
  if (opts.snapshot_dir.empty()) {
    *error = "replay: missing <snapshot_dir>";
    return std::nullopt;
  }
  return opts;
}

int RenderScreenGoldens(const std::string& dir,
                        bool rewrite,
                        std::string* report) {
  std::vector<fs::path> snaps;
  std::error_code ec;
  for (const fs::directory_entry& ent : fs::directory_iterator(dir, ec)) {
    if (ent.is_regular_file() &&
        ent.path().filename().string().ends_with(".snapshot.json")) {
      snaps.push_back(ent.path());
    }
  }
  if (ec || snaps.empty()) {
    *report = base::StrCat({"no *.snapshot.json files in ", dir, "\n"});
    return 1;
  }
  std::sort(snaps.begin(), snaps.end());

  int diffs = 0;
  std::string out;
  for (const fs::path& jf : snaps) {
    const std::string fname = jf.filename().string();
    const std::string stem = fname.substr(0, fname.size() - 14);
    const std::optional<std::string> raw = ReadFile(jf.string());
    pb::Snapshot snap;
    std::string err;
    if (!raw || !pb::ReadJson(*raw, &snap, &err)) {
      base::StrAppend(&out, {"PARSE ERROR: ", fname, ": ", err, "\n"});
      ++diffs;
      continue;
    }
    const Screen screen = RenderScreen(snap);
    const std::string want_txt = screen.text + "\n";
    std::string want_opts;
    for (OptionKind k : screen.options) {
      base::StrAppend(&want_opts, {ShortOptionTag(k), "\n"});
    }
    const fs::path txt_path = jf.parent_path() / (stem + ".txt");
    const fs::path opt_path = jf.parent_path() / (stem + ".options");
    if (rewrite) {
      if (!WriteFileAtomic(txt_path.string(), want_txt) ||
          !WriteFileAtomic(opt_path.string(), want_opts)) {
        base::StrAppend(&out, {"WRITE ERROR: ", stem, "\n"});
        ++diffs;
      }
    } else {
      if (ReadFile(txt_path.string()).value_or("") != want_txt) {
        base::StrAppend(&out, {"DIFFERS: ", stem, ".txt\n"});
        ++diffs;
      }
      if (ReadFile(opt_path.string()).value_or("") != want_opts) {
        base::StrAppend(&out, {"DIFFERS: ", stem, ".options\n"});
        ++diffs;
      }
    }
  }
  if (rewrite) {
    base::StrAppend(&out, {base::StringPrintf("rewrote %zu screen goldens\n",
                                              snaps.size())});
  } else {
    base::StrAppend(&out, {base::StringPrintf("%zu snapshots, %d differ\n",
                                              snaps.size(), diffs)});
  }
  *report = std::move(out);
  return diffs == 0 ? 0 : 1;
}

int RunReplay(
    const ReplayOptions& options,
    const base::RepeatingCallback<LlmResult(const LlmRequest&)>& caller,
    std::string* report) {
  const std::optional<std::string> rules = ReadFile(options.rules_path);
  if (!rules || rules->empty()) {
    *report =
        base::StrCat({"could not read rules: ", options.rules_path, "\n"});
    return 1;
  }
  const std::vector<BotReplayData> bots = BuildReplayWakes(options, *rules);
  if (bots.empty()) {
    *report = base::StrCat(
        {"no valid snapshot timelines in ", options.snapshot_dir, "\n"});
    return 1;
  }

  std::vector<const PreparedWake*> flat_wakes;
  for (const BotReplayData& b : bots) {
    for (const PreparedWake& w : b.wakes) {
      flat_wakes.push_back(&w);
    }
  }

  struct Stats {
    base::Lock lock;
    std::vector<double> call_s GUARDED_BY(lock);
    std::vector<double> first_s GUARDED_BY(lock);
    std::vector<double> total_wait_s GUARDED_BY(lock);
    int valid GUARDED_BY(lock) = 0;
    int total GUARDED_BY(lock) = 0;
    int failed GUARDED_BY(lock) = 0;
  };
  Stats stats;

  if (options.mode == "capacity") {
    const base::TimeTicks t0 = base::TimeTicks::Now();
    const base::TimeTicks warm_end = t0 + SecondsToDelta(options.warmup_s);
    const base::TimeTicks run_end = warm_end + SecondsToDelta(options.seconds);

    std::vector<std::unique_ptr<base::Thread>> workers;
    workers.reserve(static_cast<size_t>(options.slots));
    for (int slot = 0; slot < options.slots; ++slot) {
      auto th = std::make_unique<base::Thread>(
          base::StringPrintf("replay-cap-%d", slot));
      th->Start();
      th->task_runner()->PostTask([&, slot] {
        std::mt19937_64 rng(static_cast<uint64_t>(slot) + 1);
        std::uniform_int_distribution<size_t> pick(0, flat_wakes.size() - 1);
        while (base::TimeTicks::Now() < run_end) {
          const PreparedWake* w = flat_wakes[pick(rng)];
          LlmRequest req = w->request;
          req.slot = slot;
          const base::TimeTicks start = base::TimeTicks::Now();
          const LlmResult res = caller.Run(req);
          const base::TimeTicks end = base::TimeTicks::Now();
          if (start < warm_end) {
            continue;
          }
          base::AutoLock lock(stats.lock);
          if (!res.reply) {
            ++stats.failed;
            continue;
          }
          ++stats.total;
          const bool ok =
              ParsePlan(res.reply->text, w->option_count, {}).has_value();
          if (ok) {
            ++stats.valid;
          }
          const double dt = (end - start).InSecondsF();
          stats.call_s.push_back(dt);
          stats.first_s.push_back(
              res.first_line_ms > 0.0 ? res.first_line_ms / 1000.0 : dt);
        }
      });
      workers.push_back(std::move(th));
    }
    for (auto& th : workers) {
      th->Stop();
    }

    base::AutoLock lock(stats.lock);
    const double rate =
        options.seconds > 0.0 ? stats.total / options.seconds : 0.0;
    *report = base::StringPrintf(
        "%d workers: %.1f wake-ups/s | one call p50 %.2fs p95 %.2fs | "
        "valid %d/%d | first-line p50 %.2fs p95 %.2fs%s\n",
        options.slots, rate, Percentile(stats.call_s, 0.50),
        Percentile(stats.call_s, 0.95), stats.valid, stats.total,
        Percentile(stats.first_s, 0.50), Percentile(stats.first_s, 0.95),
        stats.failed > 0
            ? base::StringPrintf(" | %d calls FAILED (left out)", stats.failed)
                  .c_str()
            : "");
    return stats.failed == 0 && stats.total > 0 ? 0 : 1;
  }

  // Real-time load replay across `options.bots` virtual bots.
  struct Scheduled {
    double due_s = 0.0;
    bool urgent = false;
    uint64_t seq = 0;
    const PreparedWake* wake = nullptr;
    int slot = 0;
  };
  std::vector<Scheduled> schedule;
  std::mt19937_64 rng(42);
  uint64_t next_seq = 0;
  for (int b = 0; b < options.bots; ++b) {
    const BotReplayData& src = bots[static_cast<size_t>(b) % bots.size()];
    const double offset =
        (b < static_cast<int>(bots.size()))
            ? 0.0
            : std::uniform_real_distribution<double>(0.0, src.duration_s)(rng);
    for (double cycle = 0.0; cycle <= options.seconds + src.duration_s;
         cycle += src.duration_s) {
      for (const PreparedWake& w : src.wakes) {
        const double due =
            w.t_s + offset + cycle - (offset > 0.0 ? src.duration_s : 0.0);
        if (due >= 0.0 && due <= options.seconds) {
          schedule.push_back(
              {due, w.urgent, next_seq++, &w, b % options.slots});
        }
      }
    }
  }
  std::sort(
      schedule.begin(), schedule.end(),
      [](const Scheduled& a, const Scheduled& b) { return a.due_s < b.due_s; });

  struct QueueCmp {
    bool operator()(const Scheduled& a, const Scheduled& b) const {
      if (a.urgent != b.urgent) {
        return !a.urgent && b.urgent;  // Urgent wakes first.
      }
      return a.seq > b.seq;
    }
  };
  struct QueueState {
    base::Lock lock;
    std::priority_queue<Scheduled, std::vector<Scheduled>, QueueCmp> ready
        GUARDED_BY(lock);
    bool done GUARDED_BY(lock) = false;
  };
  QueueState q;

  const base::TimeTicks t0 = base::TimeTicks::Now();
  std::vector<std::unique_ptr<base::Thread>> workers;
  workers.reserve(static_cast<size_t>(options.slots));
  for (int slot = 0; slot < options.slots; ++slot) {
    auto th = std::make_unique<base::Thread>(
        base::StringPrintf("replay-load-%d", slot));
    th->Start();
    th->task_runner()->PostTask([&, slot] {
      for (;;) {
        Scheduled item;
        {
          base::AutoLock lock(q.lock);
          if (q.ready.empty()) {
            if (q.done) {
              return;
            }
          } else {
            item = q.ready.top();
            q.ready.pop();
          }
        }
        if (item.wake == nullptr) {
          base::PlatformThread::Sleep(base::Milliseconds(5));
          continue;
        }
        LlmRequest req = item.wake->request;
        req.slot = slot;
        const LlmResult res = caller.Run(req);
        const double finish_s = (base::TimeTicks::Now() - t0).InSecondsF();
        base::AutoLock lock(stats.lock);
        if (!res.reply) {
          ++stats.failed;
          continue;
        }
        ++stats.total;
        if (ParsePlan(res.reply->text, item.wake->option_count, {})
                .has_value()) {
          ++stats.valid;
        }
        stats.total_wait_s.push_back(std::max(0.0, finish_s - item.due_s));
      }
    });
    workers.push_back(std::move(th));
  }

  for (const Scheduled& item : schedule) {
    const base::TimeTicks target = t0 + SecondsToDelta(item.due_s);
    const base::TimeDelta wait = target - base::TimeTicks::Now();
    if (wait.is_positive()) {
      base::PlatformThread::Sleep(wait);
    }
    base::AutoLock lock(q.lock);
    q.ready.push(item);
  }
  {
    base::AutoLock lock(q.lock);
    q.done = true;
  }
  for (auto& th : workers) {
    th->Stop();
  }

  base::AutoLock lock(stats.lock);
  *report = base::StringPrintf(
      "wake->plan p50 %.2fs p95 %.2fs max %.2fs | valid %d/%d%s\n",
      Percentile(stats.total_wait_s, 0.50),
      Percentile(stats.total_wait_s, 0.95), Percentile(stats.total_wait_s, 1.0),
      stats.valid, stats.total,
      stats.failed > 0
          ? base::StringPrintf(" | %d calls FAILED (left out)", stats.failed)
                .c_str()
          : "");
  return stats.failed == 0 && stats.total > 0 ? 0 : 1;
}

}  // namespace harness
