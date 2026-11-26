// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "bot_manager/spell_miss_packet.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace llm_brain {
namespace {

void AppendPackedGuid(std::vector<uint8_t>* buf, uint64_t guid) {
  uint8_t mask = 0;
  uint8_t bytes[8];
  int count = 0;
  for (int i = 0; i < 8; ++i) {
    const uint8_t b = static_cast<uint8_t>((guid >> (i * 8)) & 0xffu);
    if (b != 0) {
      mask |= static_cast<uint8_t>(1u << i);
      bytes[count++] = b;
    }
  }
  buf->push_back(mask);
  for (int i = 0; i < count; ++i) {
    buf->push_back(bytes[i]);
  }
}

void AppendU32(std::vector<uint8_t>* buf, uint32_t v) {
  buf->push_back(static_cast<uint8_t>(v & 0xffu));
  buf->push_back(static_cast<uint8_t>((v >> 8) & 0xffu));
  buf->push_back(static_cast<uint8_t>((v >> 16) & 0xffu));
  buf->push_back(static_cast<uint8_t>((v >> 24) & 0xffu));
}

void AppendU64(std::vector<uint8_t>* buf, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    buf->push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xffu));
  }
}

std::vector<uint8_t> BuildSpellGoPacket(
    uint64_t item_or_caster,
    uint64_t caster,
    uint32_t spell_id,
    const std::vector<uint64_t>& hits,
    const std::vector<std::pair<uint64_t, uint8_t>>& misses) {
  std::vector<uint8_t> buf;
  AppendPackedGuid(&buf, item_or_caster);
  AppendPackedGuid(&buf, caster);
  buf.push_back(1);  // cast_count
  AppendU32(&buf, spell_id);
  AppendU32(&buf, 0x00000002u);  // cast_flags
  AppendU32(&buf, 123456u);      // timestamp
  buf.push_back(static_cast<uint8_t>(hits.size()));
  for (uint64_t h : hits) {
    AppendU64(&buf, h);
  }
  buf.push_back(static_cast<uint8_t>(misses.size()));
  for (const auto& [target, miss_info] : misses) {
    AppendU64(&buf, target);
    buf.push_back(miss_info);
    if (miss_info == 11) {  // SPELL_MISS_REFLECT has extra reflectResult byte
      buf.push_back(0);
    }
  }
  return buf;
}

TEST(SpellMissPacketTest, ParsesAllSixCanonicalMissReasons) {
  constexpr uint64_t kCaster = 0x0000000000000042ULL;
  const std::vector<std::pair<uint64_t, uint8_t>> misses = {
      {0xF130000010000001ULL, 1},  // MISS
      {0xF130000010000002ULL, 2},  // RESIST
      {0xF130000010000003ULL, 3},  // DODGE
      {0xF130000010000004ULL, 4},  // PARRY
      {0xF130000010000005ULL, 6},  // EVADE
      {0xF130000010000006ULL, 7},  // IMMUNE
      {0xF130000010000007ULL, 8},  // IMMUNE2
  };
  const std::vector<uint8_t> pkt =
      BuildSpellGoPacket(kCaster, kCaster, /*spell_id=*/6770,
                         /*hits=*/{0xF130000010000099ULL}, misses);
  const std::optional<ParsedSpellGo> parsed = ParseSpellGoMisses(pkt);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->caster_guid, kCaster);
  EXPECT_EQ(parsed->spell_id, 6770u);
  ASSERT_EQ(parsed->misses.size(), 7u);
  EXPECT_EQ(parsed->misses[0].reason, "miss");
  EXPECT_EQ(parsed->misses[1].reason, "resist");
  EXPECT_EQ(parsed->misses[2].reason, "dodge");
  EXPECT_EQ(parsed->misses[3].reason, "parry");
  EXPECT_EQ(parsed->misses[4].reason, "evade");
  EXPECT_EQ(parsed->misses[5].reason, "immune");
  EXPECT_EQ(parsed->misses[6].reason, "immune");
}

TEST(SpellMissPacketTest, SkipsBlockAbsorbAndReflectReflectByte) {
  constexpr uint64_t kItem = 0x4000000000000099ULL;
  constexpr uint64_t kCaster = 0x0000000000000042ULL;
  const std::vector<std::pair<uint64_t, uint8_t>> misses = {
      {0xF130000010000001ULL, 5},   // BLOCK (ignored)
      {0xF130000010000002ULL, 10},  // ABSORB (ignored)
      {0xF130000010000003ULL, 11},  // REFLECT (ignored, consumes extra byte)
      {0xF130000010000004ULL, 2},   // RESIST (kept after reflect!)
  };
  const std::vector<uint8_t> pkt =
      BuildSpellGoPacket(kItem, kCaster, 118, {}, misses);
  const std::optional<ParsedSpellGo> parsed = ParseSpellGoMisses(pkt);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->cast_item_or_caster_guid, kItem);
  EXPECT_EQ(parsed->caster_guid, kCaster);
  ASSERT_EQ(parsed->misses.size(), 1u);
  EXPECT_EQ(parsed->misses[0].target_guid, 0xF130000010000004ULL);
  EXPECT_EQ(parsed->misses[0].reason, "resist");
}

// Adversarial: another caster's GUID in SMSG_SPELL_GO does not match this bot's
// GUID, so the packet handler can reject non-caster broadcasts cleanly.
TEST(SpellMissPacketTest, DistinguishesOtherCasterFromSelf) {
  constexpr uint64_t kOtherCaster = 0x0000000000000077ULL;
  constexpr uint64_t kSelfBot = 0x0000000000000042ULL;
  const std::vector<uint8_t> pkt = BuildSpellGoPacket(
      kOtherCaster, kOtherCaster, 6770, {}, {{0xF130000010000001ULL, 3}});
  const std::optional<ParsedSpellGo> parsed = ParseSpellGoMisses(pkt);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_NE(parsed->caster_guid, kSelfBot);
  EXPECT_EQ(parsed->caster_guid, kOtherCaster);
}

TEST(SpellMissPacketTest, TruncatedSpellGoReturnsNullopt) {
  const std::vector<uint8_t> pkt = BuildSpellGoPacket(
      0x42, 0x42, 6770, {0x1111ULL}, {{0x2222ULL, 11}, {0x3333ULL, 3}});
  for (size_t len = 0; len < pkt.size(); ++len) {
    EXPECT_FALSE(ParseSpellGoMisses(std::span<const uint8_t>(pkt.data(), len)))
        << "len=" << len;
  }
}

TEST(SpellMissPacketTest, ParsesCastFailedAndMapsReasons) {
  std::vector<uint8_t> buf;
  buf.push_back(2);        // cast_count
  AppendU32(&buf, 6770u);  // Sap
  buf.push_back(38);       // SPELL_FAILED_IMMUNE
  const std::optional<ParsedCastFailed> parsed = ParseCastFailed(buf);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->cast_count, 2u);
  EXPECT_EQ(parsed->spell_id, 6770u);
  EXPECT_EQ(parsed->result, 38u);
  EXPECT_TRUE(IsCastFailureImmune(parsed->result));
  EXPECT_EQ(CastFailureReason(parsed->result), "immune");
  EXPECT_EQ(CastFailureReason(47), "target not in line of sight");
  EXPECT_EQ(CastFailureReason(97), "out of range");
  EXPECT_EQ(CastFailureReason(110), "target is in combat");
  EXPECT_TRUE(CastFailureReason(172).empty());  // SPELL_FAILED_DONT_REPORT

  EXPECT_FALSE(ParseCastFailed(std::span<const uint8_t>(buf.data(), 5)));
}

TEST(SpellMissPacketTest, FormatsMissLineWithAndWithoutTarget) {
  EXPECT_EQ(FormatSpellMissLine("Sap", "Defias Cutpurse", "dodge"),
            "missed: Sap on Defias Cutpurse (dodge)");
  EXPECT_EQ(FormatSpellMissLine("Polymorph", "", "immune"),
            "missed: Polymorph (immune)");
}

}  // namespace
}  // namespace llm_brain
