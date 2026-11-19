// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Small file helpers for the harness (reading rules and snapshots, appending
// to logs). POSIX calls: our base has no file API yet.

#ifndef HARNESS_RUNTIME_FILE_UTIL_H_
#define HARNESS_RUNTIME_FILE_UTIL_H_

#include <optional>
#include <string>
#include <string_view>

namespace harness {

// The whole file, or nullopt if it can't be read.
std::optional<std::string> ReadFile(const std::string& path);

// Creates `dir` and any missing parents. True if it exists afterwards.
bool MakeDirectories(const std::string& dir);

// Appends `text` to the file (created if missing) with a single write, so two
// appenders' lines don't interleave.
bool AppendToFile(const std::string& path, std::string_view text);

}  // namespace harness

#endif  // HARNESS_RUNTIME_FILE_UTIL_H_
