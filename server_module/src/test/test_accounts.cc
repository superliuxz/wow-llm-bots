// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "test/test_accounts.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace llm_brain {
namespace {

std::string Upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return s;
}

}  // namespace

std::string TestAccountName(const std::string& base, size_t index) {
  return index == 0 ? base : base + std::to_string(index + 1);
}

bool IsTestAccountName(const std::string& name, const std::string& base) {
  const std::string upper_name = Upper(name);
  const std::string upper_base = Upper(base);
  if (upper_base.empty() || upper_name.rfind(upper_base, 0) != 0) {
    return false;
  }
  const std::string suffix = upper_name.substr(upper_base.size());
  if (suffix.empty()) {
    return true;
  }
  // A number from 2 up, without a leading zero ("2", "12"; never "1", "02").
  return suffix.front() != '0' && suffix != "1" &&
         std::all_of(suffix.begin(), suffix.end(),
                     [](unsigned char c) { return std::isdigit(c); });
}

}  // namespace llm_brain
