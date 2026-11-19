// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// TEST ONLY. Whether a GBNF grammar accepts a text, checked with llama.cpp's
// own grammar code (the same that constrains llama-server), so a test can
// prove that what our printers write is what our grammars allow
// (docs/plans/grammar-idl.md, "Tests"). No model, vocabulary or GGML runtime
// is involved. This header keeps llama.cpp's headers (<map>, <regex>) out of
// the code that includes it.

#ifndef HARNESS_TEST_GBNF_CHECK_H_
#define HARNESS_TEST_GBNF_CHECK_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace harness {

struct GbnfResult {
  bool grammar_ok = false;  // The grammar parsed (and has the root).
  bool accepted = false;    // The whole text matches, from `root` to the end.
};

GbnfResult GbnfAccepts(std::string_view grammar,
                       std::string_view root,
                       std::string_view text);

// A random text the grammar allows from `root`, as a model held to it could
// write: each character is one llama.cpp's grammar code accepts next (printable
// ASCII and '\n'), and the text ends, once it may, with probability
// `stop_chance` per character. nullopt if the grammar doesn't parse or the walk
// gets stuck or reaches `max_len` (try another seed). So a test can check the
// parser reads everything the grammar lets the model write (review of
// f40b0b6, finding 18), not only what our printer writes.
std::optional<std::string> GbnfSample(std::string_view grammar,
                                      std::string_view root,
                                      uint64_t seed,
                                      size_t max_len = 400,
                                      double stop_chance = 0.3);

}  // namespace harness

#endif  // HARNESS_TEST_GBNF_CHECK_H_
