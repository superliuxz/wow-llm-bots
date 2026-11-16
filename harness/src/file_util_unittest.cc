// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "file_util.h"

#include <stdlib.h>
#include <unistd.h>

#include <optional>
#include <string>

#include "gtest/gtest.h"

namespace harness {
namespace {

// A fresh directory under the system temp dir, removed (with what the tests
// make in it) at the end.
class FileUtilTest : public testing::Test {
 protected:
  void SetUp() override {
    std::string pattern = testing::TempDir() + "harness-test-XXXXXX";
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    dir_ = pattern;
  }
  void TearDown() override {
    for (const char* path :
         {"/a/b/c/log.txt", "/a/b/c", "/a/b", "/a", "/file.txt"}) {
      const std::string full = dir_ + path;
      ::unlink(full.c_str());
      ::rmdir(full.c_str());
    }
    ::rmdir(dir_.c_str());
  }
  std::string dir_;
};

TEST_F(FileUtilTest, ReadsWhatWasWritten) {
  ASSERT_TRUE(AppendToFile(dir_ + "/file.txt", "hello\nworld\n"));
  EXPECT_EQ(ReadFile(dir_ + "/file.txt"), "hello\nworld\n");
}

TEST_F(FileUtilTest, AMissingFileIsNullopt) {
  EXPECT_EQ(ReadFile(dir_ + "/nothing.txt"), std::nullopt);
}

TEST_F(FileUtilTest, AppendKeepsWhatIsThere) {
  ASSERT_TRUE(AppendToFile(dir_ + "/file.txt", "one\n"));
  ASSERT_TRUE(AppendToFile(dir_ + "/file.txt", "two\n"));
  EXPECT_EQ(ReadFile(dir_ + "/file.txt"), "one\ntwo\n");
}

TEST_F(FileUtilTest, MakesNestedDirectoriesAndIsIdempotent) {
  const std::string nested = dir_ + "/a/b/c";
  EXPECT_TRUE(MakeDirectories(nested));
  EXPECT_TRUE(MakeDirectories(nested));
  EXPECT_TRUE(AppendToFile(nested + "/log.txt", "x"));
}

TEST_F(FileUtilTest, FailsQuietlyWhenItCant) {
  ASSERT_TRUE(AppendToFile(dir_ + "/file.txt", "x"));
  // A file is in the way of the directory.
  EXPECT_FALSE(MakeDirectories(dir_ + "/file.txt/sub"));
  EXPECT_FALSE(AppendToFile(dir_ + "/missing-dir/log.txt", "x"));
}

}  // namespace
}  // namespace harness
