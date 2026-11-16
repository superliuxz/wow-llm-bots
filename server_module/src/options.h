// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_OPTIONS_H_
#define MOD_LLM_BRAIN_OPTIONS_H_

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "proto/snapshot.pb.h"

namespace llm_brain {

// What one numbered option does, and to what.
struct OptionTarget {
  enum class Kind {
    kAttack,
    kLoot,
    kKeep,  // Keep doing what I'm doing: the body carries on.
  };
  Kind kind = Kind::kKeep;
  // The unit's guid as the snapshot writes it ("0x..."); empty for kKeep.
  std::string guid;
  std::string name;
};

std::string_view OptionKindName(OptionTarget::Kind kind);

// Adds to `snapshot` the options a player would have on this screen
// (docs/specs/snapshot.md §9) and returns what each one targets, in the same
// order. Facts only, nothing ranked or left out by judgment: one "Attack
// <name>" per hostile or neutral NPC name (the nearest of that name: the one a
// player would click), one "Loot <name>" per lootable corpse name, and always
// "Keep doing what I'm doing" last. Pure: reads base.nearby, needs no game.
std::vector<OptionTarget> AddOptions(wlb::snapshot::Snapshot* snapshot);

// The options of a bot's recent snapshots, so a Choose naming (seq, option)
// can be resolved after the bot has moved on to newer snapshots. Keeps the
// snapshots built within `window` of the newest one: by time, not by count, so
// a slow answer in combat (snapshots every 250 ms) is still resolved. One per
// bot; world thread only.
class OptionHistory {
 public:
  explicit OptionHistory(base::TimeDelta window);
  OptionHistory(const OptionHistory&) = delete;
  OptionHistory& operator=(const OptionHistory&) = delete;
  ~OptionHistory();

  void Add(uint32_t seq,
           base::TimeTicks built,
           std::vector<OptionTarget> options);

  struct Lookup {
    // Set when found.
    const OptionTarget* target = nullptr;
    // When the snapshot that offered it was built.
    base::TimeTicks snapshot_built;
    // Why not, when not found.
    std::string error;
  };
  // `option` is 1-based, as the brain sees it.
  Lookup Find(uint32_t seq, uint32_t option) const;

 private:
  struct Entry {
    uint32_t seq = 0;
    base::TimeTicks built;
    std::vector<OptionTarget> options;
  };
  const base::TimeDelta window_;
  std::deque<Entry> entries_;  // Oldest first.
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_H_
