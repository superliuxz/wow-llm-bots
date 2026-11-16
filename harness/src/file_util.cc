// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "file_util.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace harness {

std::optional<std::string> ReadFile(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return std::nullopt;
  }
  std::string data;
  char buffer[4096];
  size_t n;
  while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    data.append(buffer, n);
  }
  const bool ok = std::ferror(file) == 0;
  std::fclose(file);
  return ok ? std::optional<std::string>(std::move(data)) : std::nullopt;
}

bool MakeDirectories(const std::string& dir) {
  for (size_t i = 1; i <= dir.size(); ++i) {
    if (i != dir.size() && dir[i] != '/') {
      continue;
    }
    const std::string prefix = dir.substr(0, i);
    if (::mkdir(prefix.c_str(), 0755) != 0 && errno != EEXIST) {
      return false;
    }
  }
  struct stat info;
  return ::stat(dir.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool AppendToFile(const std::string& path, std::string_view text) {
  const int fd =
      ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    return false;
  }
  // write() may take part of the text, or be interrupted: keep going.
  bool ok = true;
  while (!text.empty()) {
    const ssize_t written = ::write(fd, text.data(), text.size());
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      ok = false;
      break;
    }
    text.remove_prefix(static_cast<size_t>(written));
  }
  ::close(fd);
  return ok;
}

}  // namespace harness
