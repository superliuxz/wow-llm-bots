// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_TEST_TEST_ACCOUNTS_H_
#define MOD_LLM_BRAIN_TEST_TEST_ACCOUNTS_H_

#include <cstddef>
#include <string>

namespace llm_brain {

// Test characters live on a pool of accounts: `base`, `base`2, `base`3, ...
// (an account holds at most 10 characters).

// The pool's `index`-th account (0: `base`, 1: `base`2, ...).
std::string TestAccountName(const std::string& base, size_t index);

// Whether `name` is one of the pool's accounts (case-insensitive: AzerothCore
// stores account names in upper case).
bool IsTestAccountName(const std::string& name, const std::string& base);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_TEST_TEST_ACCOUNTS_H_
