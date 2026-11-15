// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_BRAIN_BRIDGE_H_
#define MOD_LLM_BRAIN_BRAIN_BRIDGE_H_

#include <cstddef>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/task/task_queue.h"
#include "base/threading/thread.h"

namespace llm_brain {

// Moves messages (serialized protos, any bytes) between AzerothCore's world
// thread and the harness over a Unix socket, one length-prefixed frame each.
//
// Threads: the socket lives on a private IO thread (base::Thread, epoll pump).
// Messages for the world thread are posted to `world_queue_`, which the world
// thread pumps from WorldScript::OnUpdate via RunWorldTasks(). The world thread
// only ever posts tasks, so it never waits on the socket or the harness.
//
// One harness at a time (a new connection replaces the old one). Outbound
// messages are dropped while no harness is connected; inbound messages are
// dropped when the world thread falls more than `max_pending_messages` behind.
class BrainBridge {
 public:
  using MessageHandler = base::RepeatingCallback<void(std::string)>;

  // Run on the world thread (from RunWorldTasks()).
  struct Callbacks {
    MessageHandler on_message;
    base::RepeatingCallback<void(bool connected)> on_connection_changed;
  };

  // Depends only on common/ (no AzerothCore headers), so it is unit-tested in
  // server_module/tests/.
  BrainBridge(std::string socket_path,
              size_t max_pending_messages,
              Callbacks callbacks);
  BrainBridge(const BrainBridge&) = delete;
  BrainBridge& operator=(const BrainBridge&) = delete;
  ~BrainBridge();

  // World thread. Starts the IO thread and listens. Returns false on failure.
  bool Start();
  // World thread. Closes the socket and joins the IO thread.
  void Stop();

  // World thread. Queues `message` for the harness.
  void Send(std::string message);
  // World thread. Runs pending tasks posted from the IO thread (incoming
  // messages, connection changes). Call every world update.
  void RunWorldTasks();

  // World thread. Last known connection state.
  bool connected() const { return connected_; }

 private:
  class IoCore;  // Lives on the IO thread.

  // World thread, via world_queue_.
  void OnConnectionChanged(bool connected);

  const std::string socket_path_;
  const size_t max_pending_messages_;
  Callbacks callbacks_;
  base::scoped_refptr<base::TaskQueue> world_queue_;
  base::Thread io_thread_;
  // Owned here but created, used and destroyed only on io_thread_. Tasks that
  // use it are posted before the task that deletes it, so it outlives them;
  // raw_ptr crashes at deletion if any copy of it is still alive.
  base::raw_ptr<IoCore> io_core_ = nullptr;
  bool connected_ = false;
  base::WeakPtrFactory<BrainBridge> weak_factory_{this};
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BRAIN_BRIDGE_H_
