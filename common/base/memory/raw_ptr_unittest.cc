// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/memory/raw_ptr.h"

#include <utility>

#include "gtest/gtest.h"

namespace base {
namespace {

struct Target : RawPtrTarget {
  int value = 7;
};
struct Plain {
  int value = 3;
};

TEST(RawPtrTest, BehavesLikeAPointer) {
  Target t;
  raw_ptr<Target> p = &t;
  EXPECT_EQ(p->value, 7);
  EXPECT_EQ(p.get(), &t);
  raw_ptr<Target> copy = p;
  raw_ptr<Target> moved = std::move(copy);
  EXPECT_EQ(moved.get(), &t);
  EXPECT_FALSE(copy);
  p = nullptr;
  moved = nullptr;  // No raw_ptrs left: destroying `t` is fine.
}

TEST(RawPtrTest, PlainTypesAreNotCounted) {
  raw_ptr<Plain> p;
  {
    Plain plain;
    p = &plain;
  }  // Not a RawPtrTarget: no detection, no crash.
  p = nullptr;
}

TEST(RawPtrTest, ClearAndDeleteDropsReferenceFirst) {
  raw_ptr<Target> p = new Target();
  p.ClearAndDelete();
  EXPECT_FALSE(p);
}

TEST(RawPtrTest, MayBeDanglingIsNotCounted) {
  MayBeDangling<Target> p;
  {
    Target t;
    p = &t;
  }  // Allowed to dangle by design.
  p = nullptr;
}

TEST(RawPtrDeathTest, DestroyingPointeeWithLiveRawPtrCrashes) {
  EXPECT_DEATH(
      {
        auto* t = new Target();
        raw_ptr<Target> p = t;
        delete t;  // `p` still points to it.
      },
      "dangling raw_ptr");
}

}  // namespace
}  // namespace base
