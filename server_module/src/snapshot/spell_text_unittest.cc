// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "snapshot/spell_text.h"

#include <cstdint>
#include <optional>
#include <string>

#include "gtest/gtest.h"

namespace llm_brain {
namespace {

// The spell 433 (bread: 61 health over 18 sec) and, as another spell, 999
// (s1 = 5).
class FakeValues : public SpellValueSource {
 public:
  std::optional<std::string> Value(uint32_t spell_id,
                                   char code,
                                   int index) const override {
    if (spell_id == 999 && code == 's' && index == 1) {
      return "5";
    }
    if (spell_id != 0) {
      return std::nullopt;
    }
    if (code == 's' && index == 1) {
      return "61";
    }
    if (code == 'o' && index == 1) {
      return "61";
    }
    if (code == 'd') {
      return "18 sec";
    }
    if (code == 't' && index == 1) {
      return "3";
    }
    return std::nullopt;
  }
};

TEST(SpellTextTest, FillsCommonTokens) {
  EXPECT_EQ(
      FormatSpellText("Restores $o1 health over $d.  Must remain seated "
                      "while eating.",
                      FakeValues()),
      "Restores 61 health over 18 sec.  Must remain seated while eating.");
  EXPECT_EQ(FormatSpellText("Heals $s1 every $t1 sec.", FakeValues()),
            "Heals 61 every 3 sec.");
}

TEST(SpellTextTest, AnotherSpellsValue) {
  EXPECT_EQ(FormatSpellText("Increases damage by $999s1%.", FakeValues()),
            "Increases damage by 5%.");
}

TEST(SpellTextTest, SingularOrPluralAfterTheNumber) {
  EXPECT_EQ(FormatSpellText("Lasts $t1 $lsecond:seconds;.", FakeValues()),
            "Lasts 3 seconds.");
  EXPECT_EQ(FormatSpellText("1 $lcharge:charges;.", FakeValues()), "1 charge.");
  EXPECT_EQ(FormatSpellText("Cures $ghim:her;.", FakeValues()), "Cures him.");
}

TEST(SpellTextTest, LeavesWhatItDoesntKnowAsWritten) {
  EXPECT_EQ(FormatSpellText("Deals $s2 damage.", FakeValues()),
            "Deals $s2 damage.");
  EXPECT_EQ(FormatSpellText("${$m1*2} and $/1000;s1 and $ alone and 5$",
                            FakeValues()),
            "${$m1*2} and $/1000;s1 and $ alone and 5$");
}

// A two-record WDBC with 172 fields (enough for the enUS description at 170
// and one more locale).
std::string FakeSpellDbc() {
  constexpr uint32_t kFields = 172;
  const std::string strings = std::string("\0", 1) + "Restores $o1 health." +
                              std::string("\0", 1) + "Returns you to $z." +
                              std::string("\0", 1);
  std::string out = "WDBC";
  const auto u32 = [&out](uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
  };
  u32(2);
  u32(kFields);
  u32(kFields * 4);
  u32(static_cast<uint32_t>(strings.size()));
  const uint32_t offsets[] = {1, 22};
  const uint32_t ids[] = {433, 8690};
  for (int r = 0; r < 2; ++r) {
    for (uint32_t f = 0; f < kFields; ++f) {
      u32(f == 0 ? ids[r] : f == kSpellDbcDescriptionField ? offsets[r] : 0);
    }
  }
  return out + strings;
}

TEST(SpellTextTest, ReadsDescriptionsOfWantedSpells) {
  const std::string dbc = FakeSpellDbc();
  const auto all = ParseSpellDescriptions(dbc, {433, 8690, 1}, 0);
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all.at(433), "Restores $o1 health.");
  EXPECT_EQ(all.at(8690), "Returns you to $z.");
  EXPECT_EQ(ParseSpellDescriptions(dbc, {433}, 0).size(), 1u);
  // Locale 1 has no text: nothing.
  EXPECT_TRUE(ParseSpellDescriptions(dbc, {433}, 1).empty());
  // Not a DBC, or too few fields for the locale.
  EXPECT_TRUE(ParseSpellDescriptions("nope", {433}, 0).empty());
  EXPECT_TRUE(ParseSpellDescriptions(dbc, {433}, 5).empty());
}

}  // namespace
}  // namespace llm_brain
