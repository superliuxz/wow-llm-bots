// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/functional/callback.h"

#include <memory>
#include <string>
#include <utility>

#include "base/memory/weak_ptr.h"
#include "gtest/gtest.h"

namespace base {
namespace {

TEST(CallbackTest, OnceRunsWithBoundAndUnboundArgs) {
  OnceCallback<int(int)> cb = BindOnce([](int a, int b) { return a * b; }, 6);
  EXPECT_EQ(std::move(cb).Run(7), 42);
  EXPECT_TRUE(cb.is_null());
}

TEST(CallbackTest, OnceAcceptsMoveOnlyBoundArgs) {
  auto value = std::make_unique<std::string>("moved");
  OnceCallback<std::string()> cb = BindOnce(
      [](std::unique_ptr<std::string> s) { return *s; }, std::move(value));
  EXPECT_EQ(std::move(cb).Run(), "moved");
}

TEST(CallbackTest, RepeatingRunsMoreThanOnce) {
  int count = 0;
  RepeatingClosure cb = BindRepeating([](int* c) { ++*c; }, &count);
  RepeatingClosure copy = cb;
  cb.Run();
  copy.Run();
  EXPECT_EQ(count, 2);
}

class Counter {
 public:
  void Add(int n) { total += n; }
  int total = 0;
  WeakPtrFactory<Counter> weak_factory{this};
};

TEST(CallbackTest, WeakReceiverDropsCallAfterInvalidation) {
  Counter counter;
  RepeatingCallback<void(int)> cb =
      BindRepeating(&Counter::Add, counter.weak_factory.GetWeakPtr());
  cb.Run(3);
  counter.weak_factory.InvalidateWeakPtrs();
  cb.Run(4);
  EXPECT_EQ(counter.total, 3);
}

TEST(WeakPtrTest, InvalidatedOnFactoryDestruction) {
  WeakPtr<Counter> weak;
  {
    Counter counter;
    weak = counter.weak_factory.GetWeakPtr();
    EXPECT_TRUE(weak);
  }
  EXPECT_FALSE(weak);
}

}  // namespace
}  // namespace base
