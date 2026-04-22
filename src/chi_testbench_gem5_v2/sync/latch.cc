/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5_v2/sync/latch.hh"

#include "base/logging.hh"
#include "chi_testbench_gem5_v2/driver.hh"
#include "chi_testbench_gem5_v2/seq_thread.hh"

namespace gem5
{
namespace chi_gem5tb_v2
{

void
Latch::wait(SeqThread &t)
{
    if (pending) {
        pending = false;
        return;
    }
    if (waiter) {
        panic("Latch::wait: already has a waiter");
    }
    waiter = &t;
    t.yield_to_primary();
}

void
Latch::notify()
{
    if (waiter) {
        SeqThread *w = waiter;
        waiter = nullptr;
        // Cross-fiber wake: defer the resume to the primary event loop
        // so the notifying fiber isn't orphaned.
        w->drv().schedule_xfer_wake();
    } else {
        pending = true;
    }
}

ChiEventBus::ChiEventBus(const Params &p) : SimObject(p)
{}

ChiEventBus::~ChiEventBus()
{
    for (auto &kv : latches) {
        delete kv.second;
    }
}

Latch &
ChiEventBus::get(const std::string &name)
{
    auto it = latches.find(name);
    if (it == latches.end()) {
        it = latches.emplace(name, new Latch()).first;
    }
    return *it->second;
}

void
ChiEventBus::wait_on(const std::string &name, SeqThread &t)
{
    get(name).wait(t);
}

void
ChiEventBus::notify(const std::string &name)
{
    get(name).notify();
}

} // namespace chi_gem5tb_v2
} // namespace gem5
