// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "brain_bridge.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/message_loop/message_pump.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/task_queue.h"
#include "net/unix_socket_server.h"

namespace llm_brain {

// The socket side of the bridge. Created, used and destroyed on the IO thread.
class BrainBridge::IoCore : public base::RawPtrTarget {
 public:
  IoCore(std::string path,
         base::scoped_refptr<base::TaskQueue> world_queue,
         size_t max_pending_messages,
         base::WeakPtr<BrainBridge> bridge,
         MessageHandler on_message)
      : world_queue_(std::move(world_queue)),
        max_pending_messages_(max_pending_messages),
        bridge_(std::move(bridge)),
        on_message_(std::move(on_message)),
        server_(std::move(path),
                net::UnixSocketServer::Callbacks{
                    [this] { PostConnectionChanged(true); },
                    [this](std::string frame) { OnFrame(std::move(frame)); },
                    [this] { PostConnectionChanged(false); }}) {}

  bool Listen() { return server_.Listen(); }
  void Send(const std::string& message) { server_.SendFrame(message); }

 private:
  void OnFrame(std::string frame) {
    if (world_queue_->PendingTaskCount() >= max_pending_messages_) {
      ++dropped_inbound_;
      return;
    }
    // on_message_ runs on the world thread (it's posted there).
    world_queue_->PostTask(base::BindOnce(
        [](MessageHandler handler, std::string message) {
          handler.Run(std::move(message));
        },
        on_message_, std::move(frame)));
  }

  void PostConnectionChanged(bool connected) {
    world_queue_->PostTask(
        base::BindOnce(&BrainBridge::OnConnectionChanged, bridge_, connected));
  }

  base::scoped_refptr<base::TaskQueue> world_queue_;
  const size_t max_pending_messages_;
  // Bound to the world thread: only dereferenced by tasks run there.
  base::WeakPtr<BrainBridge> bridge_;
  MessageHandler on_message_;
  net::UnixSocketServer server_;
  uint64_t dropped_inbound_ = 0;
};

BrainBridge::BrainBridge(std::string socket_path,
                         size_t max_pending_messages,
                         Callbacks callbacks)
    : socket_path_(std::move(socket_path)),
      max_pending_messages_(max_pending_messages),
      callbacks_(std::move(callbacks)),
      world_queue_(base::TaskQueue::Create()),
      io_thread_("llm-brain-io") {}

BrainBridge::~BrainBridge() {
  Stop();
}

bool BrainBridge::Start() {
  if (io_thread_.IsRunning()) {
    return true;
  }
  if (!io_thread_.StartWithOptions({base::MessagePumpType::IO})) {
    return false;
  }
  // Create the core on the IO thread and wait for Listen(). Blocking is OK
  // here: this runs once, from OnStartup, before the world starts ticking.
  base::WaitableEvent created;
  IoCore* core = nullptr;
  const bool posted = io_thread_.task_runner()->PostTask(
      [&, bridge = weak_factory_.GetWeakPtr()] {
        core = new IoCore(socket_path_, world_queue_, max_pending_messages_,
                          bridge, callbacks_.on_message);
        if (!core->Listen()) {
          delete core;
          core = nullptr;
        }
        created.Signal();
      });
  if (!posted) {  // Never wait for a task that can't run.
    io_thread_.Stop();
    return false;
  }
  created.Wait();
  io_core_ = core;
  if (!io_core_) {
    io_thread_.Stop();
    return false;
  }
  return true;
}

void BrainBridge::Stop() {
  if (!io_thread_.IsRunning()) {
    return;
  }
  // Deleting the core closes the socket and removes the socket file. It is the
  // last task posted, so every earlier task that uses the core runs first.
  io_thread_.task_runner()->PostTask(
      [core = std::move(io_core_)]() mutable { core.ClearAndDelete(); });
  io_thread_.Stop();
  connected_ = false;
}

void BrainBridge::Send(std::string message) {
  if (!io_core_ || !connected_) {
    return;  // Nobody listening: drop here instead of hopping threads.
  }
  io_thread_.task_runner()->PostTask(
      [core = io_core_, message = std::move(message)] { core->Send(message); });
}

void BrainBridge::RunWorldTasks() {
  world_queue_->RunPendingTasks();
}

void BrainBridge::OnConnectionChanged(bool connected) {
  connected_ = connected;
  if (callbacks_.on_connection_changed) {
    callbacks_.on_connection_changed.Run(connected);
  }
}

}  // namespace llm_brain
