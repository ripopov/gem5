/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CHI_TESTBENCH_GEM5_V2_SYNC_LATCH_HH__
#define __CHI_TESTBENCH_GEM5_V2_SYNC_LATCH_HH__

#include <string>
#include <unordered_map>

#include "params/ChiGem5V2EventBus.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace chi_gem5tb_v2
{

class SeqThread;

/**
 * 1-to-1 notify/wait primitive.
 *
 * - `wait(t)`: if a pending notify exists, consume it and return
 *   immediately; otherwise record `t` as the waiter and yield the
 *   fiber. On resume, clear the waiter and return.
 * - `notify()`: if a waiter is recorded, resume it; otherwise set a
 *   pending flag that the next `wait()` consumes.
 *
 * Pending-notify semantics make cross-fiber rendezvous robust
 * regardless of fiber start-up order.
 */
class Latch
{
  public:
    void wait(SeqThread &t);
    void notify();
    bool
    is_set() const
    {
        return pending;
    }
    void
    reset()
    {
        pending = false;
    }

  private:
    SeqThread *waiter = nullptr;
    bool pending = false;
};

/**
 * Named-latch registry shared across drivers. `get(name)` lazily
 * creates a Latch per string key; drivers holding the same bus
 * reference see the same set of latches.
 */
class ChiEventBus : public SimObject
{
  public:
    using Params = ChiGem5V2EventBusParams;
    explicit ChiEventBus(const Params &p);
    ~ChiEventBus() override;

    Latch &get(const std::string &name);
    void wait_on(const std::string &name, SeqThread &t);
    void notify(const std::string &name);

  private:
    std::unordered_map<std::string, Latch *> latches;
};

} // namespace chi_gem5tb_v2
} // namespace gem5

#endif // __CHI_TESTBENCH_GEM5_V2_SYNC_LATCH_HH__
