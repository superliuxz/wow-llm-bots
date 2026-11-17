// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.
//
// Connects the harness to `mod-llm-brain`'s Unix socket (`brain.sock`):
// receives `ModuleMessage` frames (`Snapshot` and `ImmediateEvent`) on a
// dedicated epoll IO thread, records `built -> received` delivery ages from
// `monotonic_us`, posts decoded messages onto the harness's own sequence, sends
// `Command` frames out, and reconnects automatically whenever `worldserver`
// restarts.

#ifndef HARNESS_MODULE_CONNECTION_H_
#define HARNESS_MODULE_CONNECTION_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "proto/snapshot.h"

namespace harness {

class ModuleConnection {
 public:
  struct Callbacks {
    base::RepeatingCallback<void(bool connected)> on_connection_changed;
    base::RepeatingCallback<void(wlb::snapshot::Snapshot,
                                 base::TimeTicks received_at)>
        on_snapshot;
    base::RepeatingCallback<void(wlb::snapshot::ImmediateEvent,
                                 base::TimeTicks received_at)>
        on_event;
  };

  struct DeliveryStats {
    // `received_at - built_at` for snapshots carrying `monotonic_us`.
    std::vector<base::TimeDelta> receive_ages;
    uint64_t snapshots = 0;
    uint64_t events = 0;
    uint64_t unparseable = 0;
    uint64_t dropped_outbound = 0;
  };

  // Must be created, used and destroyed on a sequence (`callbacks` run there).
  ModuleConnection(std::string socket_path,
                   Callbacks callbacks,
                   base::TimeDelta retry_delay = base::Seconds(1));
  ModuleConnection(const ModuleConnection&) = delete;
  ModuleConnection& operator=(const ModuleConnection&) = delete;
  ~ModuleConnection();

  bool Start();
  void Stop();

  void SendCommand(const wlb::snapshot::Command& command);
  bool connected() const { return connected_; }
  DeliveryStats TakeDeliveryStats();

 private:
  class IoCore;
  class StatsCollector;

  void OnConnectionChanged(bool connected);
  void OnSnapshot(wlb::snapshot::Snapshot snapshot,
                  base::TimeTicks received_at);
  void OnEvent(wlb::snapshot::ImmediateEvent event,
               base::TimeTicks received_at);

  const std::string socket_path_;
  Callbacks callbacks_;
  const base::TimeDelta retry_delay_;
  base::scoped_refptr<base::SequencedTaskRunner> origin_;
  std::unique_ptr<StatsCollector> stats_;
  base::Thread io_thread_;
  base::raw_ptr<IoCore> io_core_ = nullptr;
  bool connected_ = false;
  base::WeakPtrFactory<ModuleConnection> weak_factory_{this};
};

}  // namespace harness

#endif  // HARNESS_MODULE_CONNECTION_H_
