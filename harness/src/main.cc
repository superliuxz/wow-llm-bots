// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// The brain's process. So far one command:
//   harness decide <snapshot.pb> [--name= --reason= --rules= --identity=
//                    --log-dir= --host= --port= --slot= --temperature=
//                    --dry-run]

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "decide.h"
#include "llm_client.h"

namespace {

int Usage() {
  std::fputs(
      "usage: harness decide <snapshot.pb> [--name=BOT] [--reason=TEXT]\n"
      "         [--rules=FILE] [--identity=FILE] [--log-dir=DIR] [--host=H]\n"
      "         [--port=N] [--slot=N] [--temperature=T] [--dry-run]\n",
      stderr);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || std::string(argv[1]) != "decide") {
    return Usage();
  }
  const std::vector<std::string> args(argv + 2, argv + argc);
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
