// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Shared low-level helpers used by all action executors (options and
// standing-order acts): GUID parsing, movement/pathing, raid marking, item
// equipping (dual-slot aware), spell-cast tracking, and object interaction.

#ifndef MOD_LLM_BRAIN_OPTIONS_EXEC_SHARED_H_
#define MOD_LLM_BRAIN_OPTIONS_EXEC_SHARED_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

class Creature;
class GameObject;
class Item;
class ObjectGuid;
class Player;
class Unit;
struct ItemTemplate;

namespace llm_brain {

struct ActionOutcome {
  std::string error;
  bool used = false;
  std::string info;
};

// The guid of a snapshot's "0x" + 16 hex digits; empty if malformed.
ObjectGuid ParseSnapshotGuid(std::string_view text);

// The party's last minimap ping as the bot can still use it: at most
// kPartyPingShowMs old, from someone still in its group, on its own map and
// instance; a stale one is dropped from the session. `z` is the floor at the
// ping nearest the bot's own height (a ping is a point on the 2D map).
inline constexpr uint32_t kPartyPingShowMs = 30000;
struct PartyPingSpot {
  std::string from;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  uint32_t age_ms = 0;
};
std::optional<PartyPingSpot> CurrentPartyPing(Player* bot);

// The caster's last missed spell (from SMSG_SPELL_GO or SMSG_CAST_FAILED
// SPELL_FAILED_IMMUNE) while it is still fresh (at most kSpellMissShowMs old,
// on the bot's own map and instance); a stale one is dropped from the session.
inline constexpr uint32_t kSpellMissShowMs = 6000;
struct RecentSpellMiss {
  uint32_t seq = 0;
  std::string spell;
  std::string target;
  std::string reason;
};
std::optional<RecentSpellMiss> CurrentSpellMiss(Player* bot);

// True when `bot` is allowed to cancel its own movement (never while on a taxi
// flight path, riding a vehicle, or under fear/stun/charm/controlled motion).
bool CanClearBotMovement(const Player* bot);

void RecordLastMovement(Player* bot, float x, float y, float z);
void ClearLastMovement(Player* bot);
bool WalkTo(Player* bot, float x, float y, float z);
bool WalkNextTo(Player* bot, Creature* target);
bool WalkNextTo(Player* bot, GameObject* target);
bool WalkNextTo(Player* bot, Unit* target);

// Walks `bot` toward (x, y, z) along the navmesh; returns false if no path
// exists.
bool WalkTowards(Player* bot, float x, float y, float z);

// Clears stale dead selection / Playerbots "current target" when `bot` is
// alive and in combat so Playerbots' combat engine isn't stalled by a corpse.
void ClearStaleCombatTarget(Player* bot);

// Unified raid-target marking rule (shared by options and standing-order
// `mark`): in a 5-man party anyone can mark; in a raid only leader/assistant
// can mark. Returns empty `error` on success with a descriptive `info`.
ActionOutcome MarkUnitWithIcon(Player* bot, Unit* unit, uint8_t icon);

// Picks the best valid equipment slot for `bag_item` (shared by option
// discovery, `EquipItemAction`, and `Command_Act::EQUIP`). Prefers empty or
// lower-ilvl second slot for rings, trinkets, and dual-wielding weapons.
bool PickEquipSlot(Player* bot,
                   Item* bag_item,
                   const ItemTemplate* proto,
                   uint16_t* out_dest);

// Equips `bag_item` at `(bag, slot)` using `PickEquipSlot` and verifies the
// swap by item GUID.
ActionOutcome EquipBagItem(Player* bot,
                           Item* bag_item,
                           uint8_t bag,
                           uint8_t slot,
                           std::string_view label);

size_t QueuedSpellCount(const Player* bot, uint32_t spell_id);
bool IsPreparingOrChannelingSpell(const Player* bot, uint32_t spell_id);

// Whether `bot` is currently preparing, channeling, or has a non-cancelling
// queued cast for `spell_id` (excluding SPELL_STATE_DELAYED projectiles and
// unrelated SpellQueue entries).
bool IsSpellCastPending(const Player* bot, uint32_t spell_id);

// Whether `go` is opened through the loot strategy (chest, herb, vein).
bool IsLootableObject(GameObject* go);

// Whether the bot can use `go` from where it stands.
bool IsInUseRange(Player* bot, GameObject* go);

// Clicks `go` now through playerbots' "use" action. Returns why it was
// refused, or empty when used.
std::string UseObjectNow(Player* bot, GameObject* go);

// Whether the bot is close enough to talk to `npc` / loot `creature`.
bool IsInTalkRange(Player* bot, Creature* npc);
bool IsInLootRange(Player* bot, Creature* creature);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_OPTIONS_EXEC_SHARED_H_
