// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "module_connection.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/message_loop/message_pump.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/sequenced_task_runner.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "net/unix_socket_client.h"
#include "proto/snapshot.h"

namespace harness {

namespace pb = wlb::snapshot;

class ModuleConnection::StatsCollector : public base::RawPtrTarget {
 public:
  void RecordSnapshot(int64_t monotonic_us, base::TimeTicks received_at) {
    base::AutoLock lock(lock_);
    ++stats_.snapshots;
    if (monotonic_us > 0) {
      const base::TimeTicks built =
          base::TimeTicks() + base::Microseconds(monotonic_us);
      if (received_at >= built) {
        stats_.receive_ages.push_back(received_at - built);
      }
    }
  }
  void RecordEvent() {
    base::AutoLock lock(lock_);
    ++stats_.events;
  }
  void RecordWrongVersion() {
    base::AutoLock lock(lock_);
    ++stats_.wrong_version;
  }

  void RecordUnparseable() {
    base::AutoLock lock(lock_);
    ++stats_.unparseable;
  }
  void RecordDroppedOutbound() {
    base::AutoLock lock(lock_);
    ++stats_.dropped_outbound;
  }
  DeliveryStats Take() {
    base::AutoLock lock(lock_);
    return std::exchange(stats_, {});
  }

 private:
  base::Lock lock_;
  DeliveryStats stats_ GUARDED_BY(lock_);
};

class ModuleConnection::IoCore : public base::RawPtrTarget {
 public:
  IoCore(std::string socket_path,
         base::TimeDelta retry_delay,
         base::scoped_refptr<base::SequencedTaskRunner> origin,
         base::WeakPtr<ModuleConnection> owner,
         StatsCollector* stats)
      : origin_(std::move(origin)),
        owner_(std::move(owner)),
        stats_(stats),
        client_(std::move(socket_path),
                net::UnixSocketClient::Callbacks{
                    [this] { PostConnectionChanged(true); },
                    [this](std::string frame) { OnFrame(std::move(frame)); },
                    [this] { PostConnectionChanged(false); }},
                retry_delay) {}

  void Start() { client_.Start(); }

  void Send(const std::string& bytes) {
    if (!client_.SendFrame(bytes)) {
      stats_->RecordDroppedOutbound();
    }
  }

 private:
  void PostConnectionChanged(bool connected) {
    if (origin_) {
      origin_->PostTask(base::BindOnce(&ModuleConnection::OnConnectionChanged,
                                       owner_, connected));
    }
  }

  void OnFrame(std::string frame) {
    const base::TimeTicks received_at = base::TimeTicks::Now();
    pb::ModuleMessage msg;
    if (!pb::ReadBeve(frame, &msg)) {
      stats_->RecordUnparseable();
      return;
    }
    if (msg.version != pb::kWireVersion) {
      stats_->RecordWrongVersion();
      return;
    }
    if (msg.snapshot) {
      stats_->RecordSnapshot(msg.snapshot->monotonic_us, received_at);
      if (origin_) {
        origin_->PostTask(base::BindOnce(&ModuleConnection::OnSnapshot, owner_,
                                         std::move(*msg.snapshot),
                                         received_at));
      }
    } else if (msg.event) {
      stats_->RecordEvent();
      if (origin_) {
        origin_->PostTask(base::BindOnce(&ModuleConnection::OnEvent, owner_,
                                         std::move(*msg.event), received_at));
      }
    }
  }

  base::scoped_refptr<base::SequencedTaskRunner> origin_;
  base::WeakPtr<ModuleConnection> owner_;
  const base::raw_ptr<StatsCollector> stats_;
  net::UnixSocketClient client_;
};

ModuleConnection::ModuleConnection(std::string socket_path,
                                   Callbacks callbacks,
                                   base::TimeDelta retry_delay)
    : socket_path_(std::move(socket_path)),
      callbacks_(std::move(callbacks)),
      retry_delay_(retry_delay),
      origin_(base::SequencedTaskRunner::GetCurrentDefault()),
      stats_(std::make_unique<StatsCollector>()),
      io_thread_("module-io") {}

ModuleConnection::~ModuleConnection() {
  Stop();
}

bool ModuleConnection::Start() {
  if (io_thread_.IsRunning()) {
    return true;
  }
  if (!io_thread_.StartWithOptions({base::MessagePumpType::IO})) {
    return false;
  }
  base::WaitableEvent started;
  IoCore* core = nullptr;
  const bool posted = io_thread_.task_runner()->PostTask(
      [&, owner = weak_factory_.GetWeakPtr()] {
        core = new IoCore(socket_path_, retry_delay_, origin_, owner,
                          stats_.get());
        core->Start();
        started.Signal();
      });
  if (!posted) {
    io_thread_.Stop();
    return false;
  }
  started.Wait();
  io_core_ = core;
  return true;
}

void ModuleConnection::Stop() {
  if (!io_thread_.IsRunning()) {
    return;
  }
  weak_factory_.InvalidateWeakPtrs();
  io_thread_.task_runner()->PostTask(
      [core = std::move(io_core_)]() mutable { core.ClearAndDelete(); });
  io_thread_.Stop();
  connected_ = false;
}

void ModuleConnection::SendCommand(const pb::Command& command) {
  if (!io_core_) {
    return;
  }
  pb::Command versioned = command;
  versioned.version = pb::kWireVersion;
  std::string bytes = pb::WriteBeve(versioned);
  io_thread_.task_runner()->PostTask(
      [core = io_core_, bytes = std::move(bytes)] { core->Send(bytes); });
}

ModuleConnection::DeliveryStats ModuleConnection::TakeDeliveryStats() {
  return stats_->Take();
}

void ModuleConnection::OnConnectionChanged(bool connected) {
  connected_ = connected;
  if (callbacks_.on_connection_changed) {
    callbacks_.on_connection_changed.Run(connected);
  }
}

void ModuleConnection::OnSnapshot(pb::Snapshot snapshot,
                                  base::TimeTicks received_at) {
  if (callbacks_.on_snapshot) {
    callbacks_.on_snapshot.Run(std::move(snapshot), received_at);
  }
}

void ModuleConnection::OnEvent(pb::ImmediateEvent event,
                               base::TimeTicks received_at) {
  if (callbacks_.on_event) {
    callbacks_.on_event.Run(std::move(event), received_at);
  }
}

}  // namespace harness
