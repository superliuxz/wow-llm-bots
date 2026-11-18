// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "gbnf_check.h"

#include <stdexcept>
#include <string>
#include <string_view>

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

}  // namespace harness
