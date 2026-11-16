// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

void AddLlmBrainScripts();

// AzerothCore calls Add<module_dir_name>Scripts() for each module.
void Addmod_llm_brainScripts() {
  AddLlmBrainScripts();
}
