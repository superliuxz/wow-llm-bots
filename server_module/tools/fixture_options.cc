// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Prints, as a JSON array, the options the module would offer for a snapshot
// (llm_brain::AddOptions, the code the server runs), so saved snapshots
// (eval cases, screen fixtures) show the screen a live bot sees:
//   fixture_options <snapshot.json>
// tools/snapshot/add_fixture_options.py writes them into the fixtures.

#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include "options/options.h"
#include "proto/snapshot.h"

namespace {

std::optional<std::string> ReadFileBytes(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    return std::nullopt;
  }
  std::string out;
  char buf[8192];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out.append(buf, n);
  }
  const bool bad = std::ferror(f) != 0;
  std::fclose(f);
  if (bad) {
    return std::nullopt;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: fixture_options <snapshot.json>\n");
    return 2;
  }
  const std::optional<std::string> text = ReadFileBytes(argv[1]);
  if (!text) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  wlb::snapshot::Snapshot snap;
  std::string error;
  if (!wlb::snapshot::ReadJson(*text, &snap, &error)) {
    std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
    return 1;
  }
  snap.options.clear();
  llm_brain::AddOptions(&snap);
  // Only the options: the caller keeps the rest of the file as it is.
  wlb::snapshot::Snapshot only;
  only.options = std::move(snap.options);
  const std::string json = wlb::snapshot::WriteJson(only);
  if (json.empty()) {
    std::fprintf(stderr, "%s: options can't be written as JSON\n", argv[1]);
    return 1;
  }
  std::fwrite(json.data(), 1, json.size(), stdout);
  std::fputc('\n', stdout);
  return 0;
}
