/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5/sync/latch.hh"

#include "base/logging.hh"
#include "chi_testbench_gem5/driver.hh"
#include "chi_testbench_gem5/seq_thread.hh"

namespace gem5
{
namespace chi_gem5tb
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
    // Control returns here after notify() called t.run(); waiter was
    // already cleared in notify().
}

void
Latch::notify()
{
    if (waiter) {
        SeqThread *w = waiter;
        waiter = nullptr;
        // Cross-fiber wake: if we called w->run() directly from a
        // different fiber, control would transfer to w and the
        // calling fiber would be left dangling — its notify() would
        // never return. Instead, schedule a zero-delay event in the
        // waiter's driver so w is resumed from the primary fiber
        // when the current fiber next yields.
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

} // namespace chi_gem5tb
} // namespace gem5
