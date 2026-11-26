// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/spell_miss_packet.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "base/strings/strcat.h"

namespace llm_brain {
namespace {

// WoW 3.3.5a SpellMissInfo values (SharedDefines.h).
constexpr uint8_t kSpellMissMiss = 1;
constexpr uint8_t kSpellMissResist = 2;
constexpr uint8_t kSpellMissDodge = 3;
constexpr uint8_t kSpellMissParry = 4;
constexpr uint8_t kSpellMissEvade = 6;
constexpr uint8_t kSpellMissImmune = 7;
constexpr uint8_t kSpellMissImmune2 = 8;
constexpr uint8_t kSpellMissDeflect = 9;
constexpr uint8_t kSpellMissReflect = 11;

// WoW 3.3.5a SpellCastResult values (SharedDefines.h).
constexpr uint8_t kSpellFailedAffectingCombat = 1;
constexpr uint8_t kSpellFailedBadTargets = 12;
constexpr uint8_t kSpellFailedImmune = 38;
constexpr uint8_t kSpellFailedInterrupted = 40;
constexpr uint8_t kSpellFailedInterruptedCombat = 41;
constexpr uint8_t kSpellFailedLineOfSight = 47;
constexpr uint8_t kSpellFailedNotBehind = 57;
constexpr uint8_t kSpellFailedNotInFront = 61;
constexpr uint8_t kSpellFailedNotReady = 67;
constexpr uint8_t kSpellFailedNoPower = 85;
constexpr uint8_t kSpellFailedOnlyStealthed = 95;
constexpr uint8_t kSpellFailedOutOfRange = 97;
constexpr uint8_t kSpellFailedTargetAffectingCombat = 110;
constexpr uint8_t kSpellFailedCantDoThatRightNow = 173;

class ByteReader {
 public:
  explicit ByteReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

  bool ReadU8(uint8_t* out) {
    if (pos_ + 1 > bytes_.size()) {
      return false;
    }
    *out = bytes_[pos_++];
    return true;
  }

  bool ReadU32(uint32_t* out) {
    if (pos_ + 4 > bytes_.size()) {
      return false;
    }
    *out = static_cast<uint32_t>(bytes_[pos_]) |
           (static_cast<uint32_t>(bytes_[pos_ + 1]) << 8) |
           (static_cast<uint32_t>(bytes_[pos_ + 2]) << 16) |
           (static_cast<uint32_t>(bytes_[pos_ + 3]) << 24);
    pos_ += 4;
    return true;
  }

  bool ReadU64(uint64_t* out) {
    if (pos_ + 8 > bytes_.size()) {
      return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) {
      v |= static_cast<uint64_t>(bytes_[pos_ + i]) << (i * 8);
    }
    pos_ += 8;
    *out = v;
    return true;
  }

  bool ReadPackedGuid(uint64_t* out) {
    uint8_t mask = 0;
    if (!ReadU8(&mask)) {
      return false;
    }
    uint64_t guid = 0;
    for (int i = 0; i < 8; ++i) {
      if ((mask & (1u << i)) != 0) {
        uint8_t b = 0;
        if (!ReadU8(&b)) {
          return false;
        }
        guid |= static_cast<uint64_t>(b) << (i * 8);
      }
    }
    *out = guid;
    return true;
  }

  bool Skip(size_t n) {
    if (n > bytes_.size() - pos_) {
      return false;
    }
    pos_ += n;
    return true;
  }

 private:
  std::span<const uint8_t> bytes_;
  size_t pos_ = 0;
};

}  // namespace

std::string_view SpellMissReasonName(uint8_t miss_info) {
  switch (miss_info) {
    case kSpellMissMiss:
    case kSpellMissDeflect:
      return "miss";
    case kSpellMissResist:
      return "resist";
    case kSpellMissDodge:
      return "dodge";
    case kSpellMissParry:
      return "parry";
    case kSpellMissEvade:
      return "evade";
    case kSpellMissImmune:
    case kSpellMissImmune2:
      return "immune";
    default:
      return "";
  }
}

std::string FormatSpellMissLine(std::string_view spell,
                                std::string_view target,
                                std::string_view reason) {
  if (!target.empty()) {
    return base::StrCat({"missed: ", spell, " on ", target, " (", reason, ")"});
  }
  return base::StrCat({"missed: ", spell, " (", reason, ")"});
}

bool IsCastFailureImmune(uint8_t cast_result) {
  return cast_result == kSpellFailedImmune;
}

std::string_view CastFailureReason(uint8_t cast_result) {
  switch (cast_result) {
    case kSpellFailedAffectingCombat:
      return "you are in combat";
    case kSpellFailedBadTargets:
      return "invalid target";
    case kSpellFailedImmune:
      return "immune";
    case kSpellFailedInterrupted:
    case kSpellFailedInterruptedCombat:
      return "cast interrupted";
    case kSpellFailedLineOfSight:
      return "target not in line of sight";
    case kSpellFailedNotBehind:
      return "must be behind target";
    case kSpellFailedNotInFront:
      return "must be facing target";
    case kSpellFailedNotReady:
      return "spell is not ready yet";
    case kSpellFailedNoPower:
      return "not enough mana or power";
    case kSpellFailedOnlyStealthed:
      return "must be in stealth";
    case kSpellFailedOutOfRange:
      return "out of range";
    case kSpellFailedTargetAffectingCombat:
      return "target is in combat";
    case kSpellFailedCantDoThatRightNow:
      return "cannot do that right now";
    default:
      return "";
  }
}

std::optional<ParsedSpellGo> ParseSpellGoMisses(
    std::span<const uint8_t> bytes) {
  ByteReader r(bytes);
  ParsedSpellGo out;
  if (!r.ReadPackedGuid(&out.cast_item_or_caster_guid) ||
      !r.ReadPackedGuid(&out.caster_guid) || !r.ReadU8(&out.cast_count) ||
      !r.ReadU32(&out.spell_id) || !r.ReadU32(&out.cast_flags) ||
      !r.ReadU32(&out.timestamp_ms)) {
    return std::nullopt;
  }
  uint8_t hit_count = 0;
  if (!r.ReadU8(&hit_count) || !r.Skip(static_cast<size_t>(hit_count) * 8u)) {
    return std::nullopt;
  }
  uint8_t miss_count = 0;
  if (!r.ReadU8(&miss_count)) {
    return std::nullopt;
  }
  for (uint8_t i = 0; i < miss_count; ++i) {
    uint64_t target_guid = 0;
    uint8_t miss_info = 0;
    if (!r.ReadU64(&target_guid) || !r.ReadU8(&miss_info)) {
      return std::nullopt;
    }
    if (miss_info == kSpellMissReflect) {
      uint8_t reflect_result = 0;
      if (!r.ReadU8(&reflect_result)) {
        return std::nullopt;
      }
    }
    const std::string_view reason = SpellMissReasonName(miss_info);
    if (!reason.empty()) {
      out.misses.push_back(ParsedSpellGo::Miss{
          .target_guid = target_guid,
          .reason = reason,
      });
    }
  }
  return out;
}

std::optional<ParsedCastFailed> ParseCastFailed(
    std::span<const uint8_t> bytes) {
  ByteReader r(bytes);
  ParsedCastFailed out;
  if (!r.ReadU8(&out.cast_count) || !r.ReadU32(&out.spell_id) ||
      !r.ReadU8(&out.result)) {
    return std::nullopt;
  }
  return out;
}

}  // namespace llm_brain
