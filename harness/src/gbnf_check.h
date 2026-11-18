// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// TEST ONLY. Whether a GBNF grammar accepts a text, checked with llama.cpp's
// own grammar code (the same that constrains llama-server), so a test can
// prove that what our printers write is what our grammars allow
// (docs/plans/grammar-idl.md, "Tests"). No model, vocabulary or GGML runtime
// is involved. This header keeps llama.cpp's headers (<map>, <regex>) out of
// the code that includes it.

#ifndef HARNESS_GBNF_CHECK_H_
#define HARNESS_GBNF_CHECK_H_

#include <string_view>

namespace harness {

struct GbnfResult {
  bool grammar_ok = false;  // The grammar parsed (and has the root).
  bool accepted = false;    // The whole text matches, from `root` to the end.
};

GbnfResult GbnfAccepts(std::string_view grammar,
                       std::string_view root,
                       std::string_view text);

}  // namespace harness

#endif  // HARNESS_GBNF_CHECK_H_
