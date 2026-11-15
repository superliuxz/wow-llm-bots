// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef COMMON_BASE_TASK_POST_TASK_AND_REPLY_H_
#define COMMON_BASE_TASK_POST_TASK_AND_REPLY_H_

#include <utility>

#include "base/memory/ref_counted.h"
#include "base/task/sequenced_task_runner.h"

namespace base {

// Runs `task` on `target` (a ThreadPool or SequencedTaskRunner, anything with
// PostTask(OnceClosure)) and then `reply(result)` on the calling sequence.
// Must be called from a task running on a sequence. If the calling sequence is
// gone when the result is ready, the reply is dropped.
template <typename Target, typename Task, typename Reply>
bool PostTaskAndReplyWithResult(Target& target, Task task, Reply reply) {
  scoped_refptr<SequencedTaskRunner> origin =
      SequencedTaskRunner::GetCurrentDefault();
  if (!origin) {
    return false;
  }
  return target.PostTask([task = std::move(task), reply = std::move(reply),
                          origin = std::move(origin)]() mutable {
    auto result = std::move(task)();
    origin->PostTask(
        [reply = std::move(reply), result = std::move(result)]() mutable {
          std::move(reply)(std::move(result));
        });
  });
}

}  // namespace base

#endif  // COMMON_BASE_TASK_POST_TASK_AND_REPLY_H_
