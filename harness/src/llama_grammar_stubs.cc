// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// TEST ONLY. llama-grammar.cpp refers to five functions it only calls with a
// vocabulary (tokens) or on an internal error. gbnf_check.cc builds grammars
// without one, so these stand in for llama-vocab.cpp and ggml (which would pull
// in the model runtime); reaching any of them is a bug in the check, so they
// abort loudly.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <ggml.h>
#include <llama.h>

#include "llama-impl.h"
#include "llama-vocab.h"

namespace {

[[noreturn]] void Unreachable(const char* what) {
  std::fprintf(stderr, "gbnf_check: %s reached without a vocabulary\n", what);
  std::abort();
}

}  // namespace

extern "C" void ggml_abort(const char* file, int line, const char* fmt, ...) {
  std::fprintf(stderr, "gbnf_check: ggml_abort at %s:%d (%s)\n", file, line,
               fmt);
  std::abort();
}

void llama_log_internal(ggml_log_level /*level*/, const char* /*format*/, ...) {
}

bool llama_vocab::is_eog(llama_token /*id*/) const {
  Unreachable("llama_vocab::is_eog");
}

int32_t llama_vocab::tokenize(const char* /*text*/,
                              int32_t /*text_len*/,
                              llama_token* /*tokens*/,
                              int32_t /*n_tokens_max*/,
                              bool /*add_special*/,
                              bool /*parse_special*/) const {
  Unreachable("llama_vocab::tokenize");
}

const std::string& llama_vocab::token_to_piece(llama_token /*token*/) const {
  Unreachable("llama_vocab::token_to_piece");
}
