// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Small AzerothCore helpers shared by snapshot builders and option execution.
// Forward-declares core types so this header compiles without AzerothCore
// include paths (tools/wlb check includes). World thread only.

#ifndef MOD_LLM_BRAIN_SNAPSHOT_WORLD_UTIL_H_
#define MOD_LLM_BRAIN_SNAPSHOT_WORLD_UTIL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "snapshot/snapshot.h"

class Bag;
class Item;
class Player;

namespace llm_brain {

// The area's name in the server's DBC locale, or "" if unknown.
std::string AreaName(uint32_t area_id);

// "0x00000000000001FC": the raw GUID, as the client's combat log prints it.
std::string GuidString(uint64_t raw_guid);

// True for general-purpose bags (excludes profession bags, quivers, etc.).
bool IsGeneralBag(const Bag* bag);

struct BagSlotItem {
  Item* item = nullptr;
  uint8_t bag = 0;
  uint8_t slot = 0;
  bool general_bag = true;
};

// Every item in `bot`'s backpack and equipped bags.
std::vector<BagSlotItem> BagItems(Player* bot);

// The 7 crowd-control flags the client shows on the bot's own state, and their
// packed bitmask for SnapshotSchedule change detection.
wlb::snapshot::Self::Control ReadControlState(const Player* bot);
uint8_t ControlMask(const Player* bot);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_SNAPSHOT_WORLD_UTIL_H_
