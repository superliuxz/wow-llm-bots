// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Validates snapshot JSON files against wlb::snapshot::Snapshot (strict JSON
// keys + visibility rules), or prints the reflected schema JSON (--schema-json)
// for a client-event audit (the 2026-11-15 audit tool is retired;
// docs/design.md, "Client-event coverage").

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "snapshot/snapshot.h"

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
  if (argc == 2 && std::string_view(argv[1]) == "--schema-json") {
    const std::string schema = wlb::snapshot::ReflectedSchemaJson();
    std::fwrite(schema.data(), 1, schema.size(), stdout);
    std::fputc('\n', stdout);
    return 0;
  }

  int failed = 0;
  for (int i = 1; i < argc; ++i) {
    const char* path = argv[i];
    const std::optional<std::string> text = ReadFileBytes(path);
    std::vector<std::string> errors;
    if (!text) {
      errors.push_back("cannot read file");
    } else {
      wlb::snapshot::Snapshot snap;
      std::string parse_err;
      if (!wlb::snapshot::ReadJson(*text, &snap, &parse_err)) {
        errors.push_back(parse_err);
      } else {
        errors = wlb::snapshot::ValidateSnapshotVisibility(snap);
      }
    }
    if (!errors.empty()) {
      ++failed;
      std::printf("FAIL %s\n", path);
      const size_t limit = errors.size() < 10 ? errors.size() : 10;
      for (size_t j = 0; j < limit; ++j) {
        const std::string_view msg = errors[j];
        const std::string_view clipped = msg.substr(0, 200);
        std::printf("   %.*s\n", static_cast<int>(clipped.size()),
                    clipped.data());
      }
    } else {
      std::printf("ok   %s\n", path);
    }
  }
  return failed == 0 ? 0 : 1;
}
