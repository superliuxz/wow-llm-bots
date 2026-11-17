// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "run.h"

#include <inttypes.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "bot_loop.h"
#include "file_util.h"
#include "llm_client.h"
#include "module_connection.h"
#include "proto/snapshot.pb.h"

namespace harness {
namespace {

bool SplitFlag(const std::string& arg, std::string* name, std::string* value) {
  const std::optional<std::string_view> rest = base::RemovePrefix(arg, "--");
  if (!rest) {
    return false;
  }
  const size_t eq = rest->find('=');
  *name = std::string(rest->substr(0, eq));
  *value = eq == rest->npos ? "" : std::string(rest->substr(eq + 1));
  return true;
}

void DefaultStdoutLog(const std::string& line) {
  std::fprintf(stdout, "%s\n", line.c_str());
  std::fflush(stdout);
}

// Owns all sequence-bound objects on `harness-loop` thread.
class LiveSession : public base::RawPtrTarget {
 public:
  LiveSession(const RunOptions& options,
              std::string rules,
              LogMessageFn log_fn,
              base::TimeDelta retry_delay)
      : log_fn_(std::move(log_fn)),
        llm_(options.endpoint, options.slots),
        module_(
            options.socket_path,
            ModuleConnection::Callbacks{
                [this](bool connected) {
                  loop_->OnConnectionChanged(connected);
                },
                [this](wlb::snapshot::Snapshot snap, base::TimeTicks at) {
                  loop_->OnSnapshot(std::move(snap), at);
                },
                [this](wlb::snapshot::ImmediateEvent ev, base::TimeTicks at) {
                  loop_->OnEvent(std::move(ev), at);
                }},
            retry_delay) {
    BotLoopConfig cfg;
    cfg.rules = std::move(rules);
    cfg.bots_dir = options.bots_dir;
    cfg.max_in_flight = options.slots;
    cfg.temperature = options.temperature;
    loop_ = std::make_unique<BotLoop>(
        std::move(cfg),
        [this](LlmRequest req, base::OnceCallback<void(LlmResult)> reply,
               FirstLineCallback on_first_line) {
          llm_.Ask(std::move(req), std::move(reply), std::move(on_first_line));
        },
        [this](const wlb::snapshot::Command& cmd) { module_.SendCommand(cmd); },
        log_fn_);
  }

  bool Start() {
    if (!module_.Start()) {
      return false;
    }
    ScheduleTick();
    return true;
  }

  void Shutdown() { module_.Stop(); }

 private:
  void ScheduleTick() {
    auto runner = base::SequencedTaskRunner::GetCurrentDefault();
    if (!runner) {
      return;
    }
    runner->PostDelayedTask(
        base::BindOnce(&LiveSession::OnTick, weak_factory_.GetWeakPtr()),
        base::Seconds(1));
  }

  void OnTick() {
    loop_->OnTick(base::TimeTicks::Now());
    if (++ticks_ % 60 == 0) {
      LogStatsSummary();
    }
    ScheduleTick();
  }

  void LogStatsSummary() {
    ModuleConnection::DeliveryStats s = module_.TakeDeliveryStats();
    if (s.snapshots == 0 && s.events == 0) {
      return;
    }
    std::sort(s.receive_ages.begin(), s.receive_ages.end());
    const auto pct_ms = [&](double q) {
      if (s.receive_ages.empty()) {
        return 0.0;
      }
      const size_t idx = std::min(
          s.receive_ages.size() - 1,
          static_cast<size_t>(q * static_cast<double>(s.receive_ages.size())));
      return s.receive_ages[idx].InMicroseconds() / 1000.0;
    };
    if (log_fn_) {
      log_fn_.Run(base::StringPrintf(
          "harness: last min snapshots=%" PRIu64 " events=%" PRIu64
          " (built->received p50=%.2f ms p95=%.2f ms max=%.2f ms) "
          "decisions=%" PRIu64 " commands=%" PRIu64,
          s.snapshots, s.events, pct_ms(0.5), pct_ms(0.95), pct_ms(1.0),
          loop_->decisions_completed(), loop_->commands_sent()));
    }
  }

  LogMessageFn log_fn_;
  LlmClient llm_;
  ModuleConnection module_;
  std::unique_ptr<BotLoop> loop_;
  uint64_t ticks_ = 0;
  base::WeakPtrFactory<LiveSession> weak_factory_{this};
};

}  // namespace

std::optional<RunOptions> ParseRunArgs(const std::vector<std::string>& args,
                                       std::string* error) {
  RunOptions options;
  for (const std::string& arg : args) {
    std::string name;
    std::string value;
    if (!SplitFlag(arg, &name, &value)) {
      *error = base::StrCat({"unexpected positional argument: ", arg});
      return std::nullopt;
    }
    int number = 0;
    const bool is_number = base::StringToInt(value, &number);
    double real = 0;
    const bool is_real =
        base::StringToDouble(value, &real) && std::isfinite(real);
    if (name == "socket" && !value.empty()) {
      options.socket_path = value;
    } else if (name == "rules" && !value.empty()) {
      options.rules_path = value;
    } else if (name == "bots-dir") {
      options.bots_dir = value;
    } else if (name == "host" && !value.empty()) {
      options.endpoint.host = value;
    } else if (name == "port" && is_number && number > 0 && number < 65536) {
      options.endpoint.port = number;
    } else if (name == "slots" && is_number && number >= 1 && number <= 256) {
      options.slots = static_cast<size_t>(number);
    } else if (name == "temperature" && is_real && real >= 0.0 && real <= 2.0) {
      options.temperature = static_cast<float>(real);
    } else if (name == "seconds" && is_real && real >= 0.0) {
      options.seconds = real;
    } else {
      *error = base::StrCat({"unknown or invalid option: ", arg});
      return std::nullopt;
    }
  }
  return options;
}

int RunHarness(const RunOptions& options,
               base::WaitableEvent* stop_event,
               LogMessageFn log_fn) {
  if (!log_fn) {
    log_fn = base::BindRepeating(&DefaultStdoutLog);
  }
  const std::optional<std::string> rules = ReadFile(options.rules_path);
  if (!rules) {
    log_fn.Run(base::StrCat({"can't read the rules ", options.rules_path}));
    return 1;
  }

  base::Thread loop_thread("harness-loop");
  if (!loop_thread.Start()) {
    log_fn.Run("can't start harness-loop thread");
    return 1;
  }

  base::WaitableEvent started;
  base::raw_ptr<LiveSession> session = nullptr;
  bool ok = false;
  const base::TimeDelta retry_delay =
      options.seconds > 0 && options.seconds < 5.0 ? base::Milliseconds(25)
                                                   : base::Seconds(1);
  loop_thread.task_runner()->PostTask([&] {
    auto* s = new LiveSession(options, *rules, log_fn, retry_delay);
    ok = s->Start();
    if (!ok) {
      delete s;
    } else {
      session = s;
    }
    started.Signal();
  });
  started.Wait();
  if (!ok) {
    loop_thread.Stop();
    log_fn.Run("can't start module connection");
    return 1;
  }

  log_fn.Run(base::StringPrintf(
      "harness: watching %s -> llama %s:%d (%zu slots)",
      options.socket_path.c_str(), options.endpoint.host.c_str(),
      options.endpoint.port, options.slots));

  base::WaitableEvent local_stop;
  base::WaitableEvent* waiter = stop_event ? stop_event : &local_stop;
  if (options.seconds > 0) {
    waiter->TimedWait(
        base::Milliseconds(static_cast<int64_t>(options.seconds * 1000.0)));
  } else {
    waiter->Wait();
  }

  loop_thread.task_runner()->PostTask([s = std::move(session)]() mutable {
    s->Shutdown();
    s.ClearAndDelete();
  });
  loop_thread.Stop();
  return 0;
}

}  // namespace harness
