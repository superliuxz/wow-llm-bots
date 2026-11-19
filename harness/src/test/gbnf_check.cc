// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "test/gbnf_check.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "llama-grammar.h"

namespace harness {

GbnfResult GbnfAccepts(std::string_view grammar,
                       std::string_view root,
                       std::string_view text) {
  GbnfResult result;
  const std::string grammar_text(grammar);
  const std::string root_name(root);
  // No vocabulary: llama.cpp's grammar test does the same
  // (third_party/llama.cpp/tests/test-grammar-integration.cpp).
  llama_grammar* g =
      llama_grammar_init_impl(nullptr, grammar_text.c_str(), root_name.c_str(),
                              false, nullptr, 0, nullptr, 0);
  if (g == nullptr) {
    return result;
  }
  result.grammar_ok = true;
  // llama.cpp throws when a character can't follow (it is third-party code
  // built with exceptions): this test-only wrapper is the one place that
  // catches, so no exception reaches our code. Chromium bans exceptions; the
  // reason (unwinding through our code) doesn't apply inside this function.
  try {
    llama_grammar_accept_str(*g, std::string(text));
    for (const llama_grammar_stack& stack : llama_grammar_get_stacks(g)) {
      if (stack.empty()) {
        result.accepted = true;  // A stack ran out: the grammar is complete.
        break;
      }
    }
  } catch (const std::runtime_error&) {
    result.accepted = false;
  }
  llama_grammar_free_impl(g);
  return result;
}

std::optional<std::string> GbnfSample(std::string_view grammar,
                                      std::string_view root,
                                      uint64_t seed,
                                      size_t max_len,
                                      double stop_chance) {
  const std::string grammar_text(grammar);
  const std::string root_name(root);
  llama_grammar* g =
      llama_grammar_init_impl(nullptr, grammar_text.c_str(), root_name.c_str(),
                              false, nullptr, 0, nullptr, 0);
  if (g == nullptr) {
    return std::nullopt;
  }
  // splitmix64: a test's seed gives the same text everywhere.
  uint64_t state = seed;
  const auto next = [&state] {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  };
  std::optional<std::string> out = std::string();
  // As in GbnfAccepts: llama.cpp throws on a character that can't follow.
  try {
    while (true) {
      llama_grammar_stacks& stacks = llama_grammar_get_stacks(g);
      bool may_end = false;
      for (const llama_grammar_stack& stack : stacks) {
        may_end = may_end || stack.empty();
      }
      const llama_grammar_stacks before = stacks;
      std::vector<char> allowed;
      for (int c = '\n'; c <= '~'; c = c == '\n' ? ' ' : c + 1) {
        llama_grammar_get_stacks(g) = before;
        llama_grammar_accept(g, static_cast<uint32_t>(c));
        if (!llama_grammar_get_stacks(g).empty()) {
          allowed.push_back(static_cast<char>(c));
        }
      }
      llama_grammar_get_stacks(g) = before;
      if (may_end && (allowed.empty() || static_cast<double>(next() % 1000) <
                                             stop_chance * 1000)) {
        break;
      }
      if (allowed.empty() || out->size() >= max_len) {
        out.reset();
        break;
      }
      const char c = allowed[next() % allowed.size()];
      llama_grammar_accept(g, static_cast<uint32_t>(c));
      out->push_back(c);
    }
  } catch (const std::runtime_error&) {
    out.reset();
  }
  llama_grammar_free_impl(g);
  return out;
}

}  // namespace harness
