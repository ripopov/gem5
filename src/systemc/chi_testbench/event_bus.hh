/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SYSTEMC_CHI_TESTBENCH_EVENT_BUS_HH__
#define __SYSTEMC_CHI_TESTBENCH_EVENT_BUS_HH__

#include <string>
#include <unordered_map>

#include "params/ChiEventBus.hh"
#include "systemc/ext/core/sc_event.hh"
#include "systemc/ext/core/sc_module.hh"
#include "systemc/ext/core/sc_module_name.hh"

namespace gem5
{
namespace chi_testbench
{

/**
 * Shared rendezvous for SystemC drivers that need cross-tile
 * synchronisation.
 *
 * Drivers call `get(name)` to obtain an `sc_event` by string key;
 * multiple drivers using the same bus and the same name see the same
 * event.  Standard SystemC semantics apply: `wait(ev)` suspends the
 * caller until some thread invokes `ev.notify()`. Late waiters miss
 * the notify — scenarios must be structured so the waiter is blocked
 * before the notifier fires.
 *
 * Helper methods `wait_on(name)` and `notify(name)` are provided for
 * the common case where the driver does not want to cache the
 * `sc_event&` itself.
 */
class ChiEventBus : public sc_core::sc_module
{
  public:
    using Params = ChiEventBusParams;
    ChiEventBus(const Params &p, const sc_core::sc_module_name &mn);
    ~ChiEventBus() override;

    sc_core::sc_event &get(const std::string &name);
    void wait_on(const std::string &name);
    void notify(const std::string &name);

  private:
    std::unordered_map<std::string, sc_core::sc_event *> events;
};

} // namespace chi_testbench
} // namespace gem5

#endif // __SYSTEMC_CHI_TESTBENCH_EVENT_BUS_HH__
