// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#ifndef MOD_LLM_BRAIN_BRIDGE_BRAIN_BRIDGE_H_
#define MOD_LLM_BRAIN_BRIDGE_BRAIN_BRIDGE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/task/task_queue.h"
#include "base/threading/thread.h"
#include "base/time/time.h"

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
//
// Two ways to send. Send() is for events: every one must arrive, in order.
// SendLatest() is for state that goes stale (a bot's snapshot): if the harness
// is behind and the socket is full, a newer message for the same key replaces
// the waiting one, so the harness catches up on the newest state, not on old
// state it would only discard.
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

  // World thread. Queues `message` for the harness, after everything sent
  // before it. When `flush_key` is non-empty, flushes any waiting `SendLatest`
  // frame for that key first so an alerting event never overtakes the snapshot
  // sent right before it.
  void Send(std::string message, std::string flush_key = {});

  // World thread. Sends `message` now, unless the socket is full: then it
  // waits, and a newer message with the same `key` replaces it.
  void SendLatest(std::string key, std::string message);

  // What sending cost since the last call. World thread.
  struct SendStats {
    // From Send() / SendLatest() to the frame being handed to the socket: the
    // hop to the IO thread and, for SendLatest(), any wait for a full socket.
    std::vector<base::TimeDelta> delays;
    // Messages replaced by a newer one before they could be sent.
    uint64_t replaced = 0;
    // Messages the socket refused (no harness, or its buffer full).
    uint64_t dropped = 0;
    // Harness commands dropped because the world thread fell behind (its
    // queue held max_pending_messages): the harness never sees them done.
    uint64_t dropped_inbound = 0;
  };
  SendStats TakeSendStats();
  // World thread. Runs pending tasks posted from the IO thread (incoming
  // messages, connection changes). Call every world update.
  void RunWorldTasks();

  // World thread. Last known connection state.
  bool connected() const { return connected_; }

 private:
  struct IoCoreHandle : public base::RawPtrTarget {
    virtual ~IoCoreHandle() = default;
  };
  class IoCore;              // Lives on the IO thread.
  class SendStatsCollector;  // Written on the IO thread, read on the world's.
  class Generation;          // Bumped on the IO thread, read on the world's.

  // World thread, via world_queue_.
  void OnConnectionChanged(bool connected);

  const std::string socket_path_;
  const size_t max_pending_messages_;
  Callbacks callbacks_;
  base::scoped_refptr<base::TaskQueue> world_queue_;
  // Outlives the IO thread's use of it: Stop() joins the thread first.
  std::unique_ptr<SendStatsCollector> send_stats_;
  // Which harness connection is current. An inbound message from an earlier
  // one (it disconnected while its frames waited for the world thread) is
  // dropped, never applied for the next harness.
  std::unique_ptr<Generation> generation_;
  base::Thread io_thread_;
  // Owned here but created, used and destroyed only on io_thread_. Tasks that
  // use it are posted before the task that deletes it, so it outlives them;
  // raw_ptr crashes at deletion if any copy of it is still alive. Uses complete
  // base IoCoreHandle so raw_ptr<T> never inspects an incomplete type.
  base::raw_ptr<IoCoreHandle> io_core_ = nullptr;
  bool connected_ = false;
  base::WeakPtrFactory<BrainBridge> weak_factory_{this};
};

}  // namespace llm_brain

#endif  // MOD_LLM_BRAIN_BRIDGE_BRAIN_BRIDGE_H_
