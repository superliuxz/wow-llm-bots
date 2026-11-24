// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// The brain's process:
//   harness run    [--socket= --rules= --bots-dir= --host= --port=
//                   --slots= --temperature= --seconds=]
//   harness decide <snapshot.json> [--name= --goal= --reason= --rules=
//                   --identity= --log-dir= --host= --port= --slot=
//                   --temperature= --dry-run]
//   harness parse-plans  < records (CheckPlanRecords, plan.h)

#include <signal.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "base/synchronization/waitable_event.h"
#include "base/threading/thread.h"
#include "llm/llm_client.h"
#include "plan/plan.h"
#include "runtime/decide.h"
#include "runtime/run.h"

namespace {

int g_signal_write_fd = -1;

void OnShutdownSignal(int /*sig*/) {
  if (g_signal_write_fd >= 0) {
    const char byte = 1;
    const ssize_t unused = ::write(g_signal_write_fd, &byte, 1);
    (void)unused;
  }
}

int Usage() {
  std::fputs(
      "usage:\n"
      "  harness run [--socket=PATH] [--rules=FILE] [--bots-dir=DIR]\n"
      "              [--notes-dir=DIR] [--knowledge=FILE] [--host=H] "
      "[--port=N]\n"
      "              [--slots=N] [--temperature=T] [--seconds=S]\n"
      "              [--only-bots=NAME,NAME]\n"
      "  harness decide <snapshot.json> [--name=BOT] [--goal=TEXT]\n"
      "                 [--reason=TEXT] [--rules=FILE] [--identity=FILE]\n"
      "                 [--knowledge=FILE] [--log-dir=DIR] [--host=H]\n"
      "                 [--port=N] [--slot=N] [--temperature=T] [--dry-run]\n"
      "                 [--inspect-prompt] [--print-request] [--json]\n"
      "                 [--whisperers=NAME,NAME]\n"
      "  harness parse-plans < records   (\"<options>\\t<reply>\" each, NUL-"
      "separated)\n",
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

  int pipe_fds[2] = {-1, -1};
  if (::pipe(pipe_fds) != 0) {
    return 1;
  }
  g_signal_write_fd = pipe_fds[1];
  std::signal(SIGINT, &OnShutdownSignal);
  std::signal(SIGTERM, &OnShutdownSignal);

  base::WaitableEvent stop_event;
  base::Thread signal_reader("signal-reader");
  signal_reader.Start();
  const int read_fd = pipe_fds[0];
  signal_reader.task_runner()->PostTask([read_fd, &stop_event] {
    char byte = 0;
    if (::read(read_fd, &byte, 1) > 0 && byte == 1) {
      stop_event.Signal();
    }
  });

  const int code = harness::RunHarness(*options, &stop_event);

  std::signal(SIGINT, SIG_DFL);
  std::signal(SIGTERM, SIG_DFL);
  g_signal_write_fd = -1;
  const char wake = 0;
  const ssize_t unused = ::write(pipe_fds[1], &wake, 1);
  (void)unused;
  signal_reader.Stop();
  ::close(pipe_fds[0]);
  ::close(pipe_fds[1]);
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
  // Ignore SIGPIPE so a closed HTTP keep-alive or peer socket returns EPIPE
  // instead of terminating the harness process.
  std::signal(SIGPIPE, SIG_IGN);
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
  if (sub == "parse-plans") {
    std::string input;
    char buffer[4096];
    size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
      input.append(buffer, n);
    }
    std::fputs(harness::CheckPlanRecords(input).c_str(), stdout);
    return 0;
  }
  return Usage();
}
