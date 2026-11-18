// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_TEST_FACTS_DUMP_H_
#define MOD_LLM_BRAIN_TEST_FACTS_DUMP_H_

#include <string>

namespace llm_brain {

class WorldPlaces;

// TEST ONLY (LlmBrain.TestFactsQueries). Writes the OptionFacts of saved eval
// snapshots with the module's own code, so the eval's options are the ones a
// live bot at that spot would get (docs/plans/options.md, "One code path").
//
// `queries_path` holds one query per line, tab-separated:
//   snapshot.json  out.facts.json  HORDE|ALLIANCE  has_food  has_drink
//   herbalism_skill  mining_skill
// The position, map, level and class come from the snapshot; the team and the
// scene's bags and skills from the query (a fixture's hand-built scene states
// them; the snapshot doesn't show them). NPC flags come from the creature
// template of each friendly unit's guid. The walkable-ray Explore fallback
// needs a live bot and is skipped. Returns the number of files written, or -1
// with `error` set. World thread, after startup.
int DumpFixtureFacts(const WorldPlaces& world,
                     const std::string& queries_path,
                     std::string* error);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_TEST_FACTS_DUMP_H_
