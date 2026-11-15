// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "base/task/sequenced_task_runner.h"
#include "base/memory/ref_counted.h"

namespace base {
namespace {
thread_local SequencedTaskRunner* g_current_default = nullptr;
}  // namespace

// static
scoped_refptr<SequencedTaskRunner> SequencedTaskRunner::GetCurrentDefault() {
  return scoped_refptr<SequencedTaskRunner>(g_current_default);
}

// static
bool SequencedTaskRunner::HasCurrentDefault() {
  return g_current_default != nullptr;
}

SequencedTaskRunner::ScopedCurrentDefault::ScopedCurrentDefault(
    SequencedTaskRunner* runner)
    : previous_(g_current_default) {
  g_current_default = runner;
}

SequencedTaskRunner::ScopedCurrentDefault::~ScopedCurrentDefault() {
  g_current_default = previous_;
}

}  // namespace base
