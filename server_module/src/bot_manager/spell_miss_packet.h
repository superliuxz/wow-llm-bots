// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Pure WoW 3.3.5a wire parsers for SMSG_SPELL_GO (miss list) and
// SMSG_CAST_FAILED (cast result), plus canonical formatting for the caster's
// `missed: <spell> on <target> (<reason>)` line. No AzerothCore headers so the
// parser and reason mapping can be unit-tested standalone.

#ifndef MOD_LLM_BRAIN_BOT_MANAGER_SPELL_MISS_PACKET_H_
#define MOD_LLM_BRAIN_BOT_MANAGER_SPELL_MISS_PACKET_H_

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace llm_brain {

// Maps a WoW 3.3.5a SpellMissInfo byte to one of:
//   "miss", "dodge", "parry", "resist", "immune", "evade"
// Returns empty string_view for non-misses (NONE, BLOCK, ABSORB, REFLECT,
// unknown).
std::string_view SpellMissReasonName(uint8_t miss_info);

// Formats the screen/error text for a missed spell:
//   "missed: <spell> on <target> (<reason>)"
// or "missed: <spell> (<reason>)" when `target` is empty.
std::string FormatSpellMissLine(std::string_view spell,
                                std::string_view target,
                                std::string_view reason);

// True when a WoW 3.3.5a SpellCastResult byte is SPELL_FAILED_IMMUNE (38).
bool IsCastFailureImmune(uint8_t cast_result);

// Maps common WoW 3.3.5a SpellCastResult bytes from SMSG_CAST_FAILED to short
// plain-English reasons; returns empty string_view for silent/unmapped codes.
std::string_view CastFailureReason(uint8_t cast_result);

struct ParsedSpellGo {
  struct Miss {
    uint64_t target_guid = 0;
    std::string_view reason;
    bool operator==(const Miss&) const = default;
  };

  uint64_t cast_item_or_caster_guid = 0;
  uint64_t caster_guid = 0;
  uint8_t cast_count = 0;
  uint32_t spell_id = 0;
  uint32_t cast_flags = 0;
  uint32_t timestamp_ms = 0;
  std::vector<Miss> misses;

  bool operator==(const ParsedSpellGo&) const = default;
};

// Parses the header, hit list, and miss list of a WoW 3.3.5a SMSG_SPELL_GO
// payload (Spell::SendSpellGo / WriteSpellGoTargets). Returns nullopt if the
// buffer is truncated or malformed.
std::optional<ParsedSpellGo> ParseSpellGoMisses(std::span<const uint8_t> bytes);

struct ParsedCastFailed {
  uint8_t cast_count = 0;
  uint32_t spell_id = 0;
  uint8_t result = 0;

  bool operator==(const ParsedCastFailed&) const = default;
};

// Parses the fixed 6-byte prefix of a WoW 3.3.5a SMSG_CAST_FAILED payload
// (Spell::WriteCastResultInfo). Returns nullopt if shorter than 6 bytes.
std::optional<ParsedCastFailed> ParseCastFailed(std::span<const uint8_t> bytes);

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BOT_MANAGER_SPELL_MISS_PACKET_H_
