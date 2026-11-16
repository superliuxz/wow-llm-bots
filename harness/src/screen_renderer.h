// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Writes a bot's snapshot as the compact plain text the brain reads (its
// "screen"). The snapshot stays complete; this only decides how it is
// written for an LLM, which reads text tokens: one short line per unit or
// object in plain words, defaults left out (full health, in line of sight,
// idle ...), units that look the same as one line, far units that only show
// a model summarised by count. Nothing a bot could act on is dropped, except
// raw coordinates and GUIDs (distance and compass direction stay).
//
// The entries the bot can act on carry their option number, so the screen is
// also the menu the brain picks from (docs/specs/triggers.md). Until the
// module sends a real options list, the options are derived from the entries:
// attack, talk, loot, gather, use, plus a few that are always there.
//
// Ported from the Python prototype tools/bench/prompt_render.py, and tested
// against its output on real snapshots (testdata/screens). Not rendered yet,
// because the module does not fill them yet: auras, services, group, boss, pet
// and vehicle parts of a unit, a unit's cast, `doing`, `ui`, `tooltips`, and
// every extension, event and option the snapshot can carry.

#ifndef HARNESS_SCREEN_RENDERER_H_
#define HARNESS_SCREEN_RENDERER_H_

#include <string>
#include <vector>

#include "proto/snapshot.pb.h"

namespace harness {

// What a numbered option does.
enum class OptionKind {
  kAttack,
  kTalk,
  kLoot,
  kGather,
  kUse,
  kKeep,     // Keep doing what I'm doing.
  kExplore,  // Explore somewhere new.
  kRest,
  kTown,  // Go to the nearest town.
};

struct Screen {
  std::string text;
  // options[i] is what option number i + 1 does.
  std::vector<OptionKind> options;
};

Screen RenderScreen(const wlb::snapshot::Snapshot& snapshot);

}  // namespace harness

#endif  // HARNESS_SCREEN_RENDERER_H_
