// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Console commands compiled only into the isolated test server
// (`LLM_BRAIN_TEST_HOOKS`): lets e2e tests set up groups, apply/clear test
// auras, and teach spells without needing a logged-in GM client.

#ifndef SERVER_MODULE_TEST_TEST_COMMANDS_H_
#define SERVER_MODULE_TEST_TEST_COMMANDS_H_

namespace llm_brain {

void AddLlmBrainTestCommandScripts();

}  // namespace llm_brain

#endif  // SERVER_MODULE_TEST_TEST_COMMANDS_H_
