/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CHI_TESTBENCH_GEM5_SYNC_LATCH_HH__
#define __CHI_TESTBENCH_GEM5_SYNC_LATCH_HH__

#include <string>
#include <unordered_map>

#include "params/ChiGem5EventBus.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace chi_gem5tb
{

class SeqThread;

/**
 * 1-to-1 notify/wait primitive. Semantics:
 *
 * - `wait(t)`: if a pending notify exists, consume it and return
 *   immediately; otherwise record `t` as the waiter and yield the
 *   fiber. On resume, clear the waiter and return.
 * - `notify()`: if a waiter is recorded, resume it; otherwise set a
 *   "pending" flag that the next `wait()` consumes.
 *
 * Pending-notify semantics (rather than strict edge-triggered like
 * sc_event) make cross-fiber rendezvous robust against the
 * unspecified fiber start-up order — the scenario author doesn't
 * have to guarantee "waiter blocks before notifier fires".
 *
 * A Latch handles at most one waiter at a time; a second wait() with
 * a waiter already present panics. Scenarios that need broadcast
 * semantics should use one Latch per waiter (or extend this class).
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
 * Named-latch registry shared across drivers.
 *
 * `get(name)` lazily creates a Latch per string key; drivers holding
 * the same bus reference see the same set of latches. Convenience
 * `wait_on` and `notify` helpers spare the caller from holding a
 * Latch& around. Analog of the SystemC ChiEventBus.
 */
class ChiEventBus : public SimObject
{
  public:
    using Params = ChiGem5EventBusParams;
    explicit ChiEventBus(const Params &p);
    ~ChiEventBus() override;

    Latch &get(const std::string &name);
    void wait_on(const std::string &name, SeqThread &t);
    void notify(const std::string &name);

  private:
    std::unordered_map<std::string, Latch *> latches;
};

} // namespace chi_gem5tb
} // namespace gem5

#endif // __CHI_TESTBENCH_GEM5_SYNC_LATCH_HH__
