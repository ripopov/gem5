/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "systemc/chi_testbench/event_bus.hh"

namespace gem5
{
namespace chi_testbench
{

ChiEventBus::ChiEventBus(const Params &p, const sc_core::sc_module_name &mn)
    : sc_core::sc_module(mn)
{}

ChiEventBus::~ChiEventBus()
{
    for (auto &kv : events) {
        delete kv.second;
    }
}

sc_core::sc_event &
ChiEventBus::get(const std::string &name)
{
    auto it = events.find(name);
    if (it == events.end()) {
        // Give each bus-hosted event a qualified SystemC name so it
        // shows up distinctly in traces.
        const std::string qualified = std::string(this->name()) + "." + name;
        auto *ev = new sc_core::sc_event(qualified.c_str());
        it = events.emplace(name, ev).first;
    }
    return *it->second;
}

void
ChiEventBus::wait_on(const std::string &name)
{
    sc_core::wait(get(name));
}

void
ChiEventBus::notify(const std::string &name)
{
    get(name).notify();
}

} // namespace chi_testbench
} // namespace gem5

gem5::chi_testbench::ChiEventBus *
gem5::ChiEventBusParams::create() const
{
    return new gem5::chi_testbench::ChiEventBus(
        *this, sc_core::sc_module_name(name.c_str()));
}
