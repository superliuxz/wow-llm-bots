// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "gtest/gtest.h"

namespace base {
namespace {

TEST(StringsTest, StrCatAndStrAppend) {
  std::string out = StrCat({"hello", ", ", "world"});
  EXPECT_EQ(out, "hello, world");
  StrAppend(&out, {"! ", NumberToString(42)});
  EXPECT_EQ(out, "hello, world! 42");
  EXPECT_EQ(StrCat({}), "");
}

TEST(StringsTest, NumberConversions) {
  EXPECT_EQ(NumberToString(-12345), "-12345");
  EXPECT_EQ(NumberToString(uint64_t{18446744073709551615ull}),
            "18446744073709551615");

  int n = 0;
  EXPECT_TRUE(StringToInt("  -42 ", &n));
  EXPECT_EQ(n, -42);
  EXPECT_TRUE(StringToInt("+7", &n));
  EXPECT_EQ(n, 7);
  EXPECT_FALSE(StringToInt("", &n));
  EXPECT_FALSE(StringToInt("12abc", &n));
  EXPECT_FALSE(StringToInt("99999999999999999999", &n));

  unsigned int u = 0;
  EXPECT_FALSE(StringToUint("-1", &u));
  EXPECT_TRUE(StringToUint("99", &u));
  EXPECT_EQ(u, 99u);

  double d = 0;
  EXPECT_TRUE(StringToDouble(" 0.75 ", &d));
  EXPECT_DOUBLE_EQ(d, 0.75);
  EXPECT_FALSE(StringToDouble("abc", &d));
}

TEST(StringsTest, CasePrefixesTrimAndJoin) {
  EXPECT_EQ(ToLowerASCII("AbC_123!"), "abc_123!");
  EXPECT_EQ(ToUpperASCII("AbC_123!"), "ABC_123!");
  EXPECT_TRUE(EqualsCaseInsensitiveASCII("Attack", "aTTaCk"));
  EXPECT_FALSE(EqualsCaseInsensitiveASCII("Attack", "Attacks"));

  EXPECT_TRUE(StartsWith("Explore town", "Explore"));
  EXPECT_FALSE(StartsWith("explore town", "Explore"));
  EXPECT_TRUE(
      StartsWith("explore town", "Explore", CompareCase::INSENSITIVE_ASCII));
  EXPECT_TRUE(EndsWith("hp<40%", "%"));

  EXPECT_EQ(RemovePrefix("data: [DONE]", "data:"),
            std::optional<std::string_view>(" [DONE]"));
  EXPECT_FALSE(RemovePrefix("event: ping", "data:"));
  EXPECT_EQ(RemoveSuffix("40yd", "yd"), std::optional<std::string_view>("40"));

  EXPECT_EQ(TrimWhitespaceASCII(" \t\r\n hi \n "), "hi");
  EXPECT_EQ(TrimWhitespaceASCII("   "), "");
  EXPECT_EQ(TrimString("..hi!?", ".!?", TRIM_ALL), "hi");
  EXPECT_EQ(TrimString("..hi..", ".", TRIM_LEADING), "hi..");
  EXPECT_EQ(TrimString("..hi..", ".", TRIM_TRAILING), "..hi");

  EXPECT_EQ(JoinString(std::vector<std::string>{"a", "b", "c"}, ", "),
            "a, b, c");
  EXPECT_EQ(JoinString(std::vector<std::string>{}, ", "), "");
}

TEST(StringsTest, SplitStringPieceUsingSubstr) {
  EXPECT_EQ(
      SplitStringPieceUsingSubstr("away, then to boss, then wake", ", then ",
                                  KEEP_WHITESPACE, SPLIT_WANT_ALL),
      (std::vector<std::string_view>{"away", "to boss", "wake"}));
  // A comma alone is not the delimiter.
  EXPECT_EQ(SplitStringPieceUsingSubstr("a, b", ", then ", KEEP_WHITESPACE,
                                        SPLIT_WANT_ALL),
            (std::vector<std::string_view>{"a, b"}));
  EXPECT_EQ(SplitStringPieceUsingSubstr(" x &&  & y ", "&", TRIM_WHITESPACE,
                                        SPLIT_WANT_NONEMPTY),
            (std::vector<std::string_view>{"x", "y"}));
  EXPECT_EQ(
      SplitStringPieceUsingSubstr("", "&", KEEP_WHITESPACE, SPLIT_WANT_ALL),
      (std::vector<std::string_view>{""}));
}

TEST(StringsTest, SplitStringAndSplitStringPiece) {
  EXPECT_EQ(
      SplitStringPiece(" 1, 2 , , 3 ", ",", TRIM_WHITESPACE, SPLIT_WANT_ALL),
      (std::vector<std::string_view>{"1", "2", "", "3"}));
  EXPECT_EQ(SplitString("a\n\nb\n", "\n", KEEP_WHITESPACE, SPLIT_WANT_NONEMPTY),
            (std::vector<std::string>{"a", "b"}));
  EXPECT_TRUE(
      SplitStringPiece("", ",", TRIM_WHITESPACE, SPLIT_WANT_ALL).empty());
}

TEST(StringsTest, StringPrintfSmallAndLarge) {
  EXPECT_EQ(StringPrintf("%04d-%02d-%02d", 2026, 11, 16), "2026-11-16");
  const std::string big(2000, 'x');
  const std::string formatted = StringPrintf("[%s:%d]", big.c_str(), 9);
  EXPECT_EQ(formatted.size(), 2004u);
  EXPECT_TRUE(StartsWith(formatted, "[xxxx"));
  EXPECT_TRUE(EndsWith(formatted, "xxxx:9]"));
}

}  // namespace
}  // namespace base
