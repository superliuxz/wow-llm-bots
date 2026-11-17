// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// The brain's process:
//   harness run    [--socket= --rules= --bots-dir= --host= --port=
//                   --slots= --temperature= --seconds=]
//   harness decide <snapshot.pb> [--name= --goal= --reason= --rules=
//                   --identity= --log-dir= --host= --port= --slot=
//                   --temperature= --dry-run]

#include <pthread.h>  // IWYU pragma: keep
#include <signal.h>
#include <time.h>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "base/synchronization/waitable_event.h"
#include "base/threading/thread.h"
#include "decide.h"
#include "llm_client.h"
#include "run.h"

namespace {

int Usage() {
  std::fputs(
      "usage:\n"
      "  harness run [--socket=PATH] [--rules=FILE] [--bots-dir=DIR]\n"
      "              [--host=H] [--port=N] [--slots=N] [--temperature=T]\n"
      "              [--seconds=S]\n"
      "  harness decide <snapshot.pb> [--name=BOT] [--goal=TEXT]\n"
      "                 [--reason=TEXT] [--rules=FILE] [--identity=FILE]\n"
      "                 [--log-dir=DIR] [--host=H] [--port=N] [--slot=N]\n"
      "                 [--temperature=T] [--dry-run]\n",
      stderr);
  return 2;
}

int RunCommand(const std::vector<std::string>& args) {
  std::string error;
  const std::optional<harness::RunOptions> options =
      harness::ParseRunArgs(args, &error);
  if (!options) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return Usage();
  }

  sigset_t mask{};
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &mask, nullptr);

  base::WaitableEvent stop_event;
  base::WaitableEvent finished;
  base::Thread sig_thread("sig-watch");
  sig_thread.Start();
  sig_thread.task_runner()->PostTask([&] {
    while (!finished.IsSignaled()) {
      timespec ts{0, 100 * 1000 * 1000};  // 100 ms
      if (sigtimedwait(&mask, nullptr, &ts) > 0) {
        stop_event.Signal();
        break;
      }
    }
  });

  const int code = harness::RunHarness(*options, &stop_event);
  finished.Signal();
  sig_thread.Stop();
  return code;
}

int DecideCommand(const std::vector<std::string>& args) {
  std::string error;
  const std::optional<harness::DecideOptions> options =
      harness::ParseDecideArgs(args, &error);
  if (!options) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return Usage();
  }
  const harness::LlmEndpoint endpoint = options->endpoint;
  std::string report;
  const int code = harness::RunDecide(
      *options,
      [endpoint](const harness::LlmRequest& request) {
        return harness::CallLlama(endpoint, request);
      },
      &report);
  std::fputs(report.c_str(), stdout);
  return code;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return Usage();
  }
  const std::string sub(argv[1]);
  const std::vector<std::string> args(argv + 2, argv + argc);
  if (sub == "run") {
    return RunCommand(args);
  }
  if (sub == "decide") {
    return DecideCommand(args);
  }
  return Usage();
}
