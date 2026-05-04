/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5/sequences/ping_pong.hh"

#include <vector>

#include "base/logging.hh"
#include "base/trace.hh"
#include "chi_testbench_gem5/driver.hh"
#include "debug/ChiTestbenchGem5.hh"

namespace gem5
{
namespace chi_gem5tb
{

void
PingPongSequence::run(ChiSeqDriver &drv)
{
    if (!drv.event_bus()) {
        panic("%s ping_pong: event_bus param is required", drv.name());
    }
    const uint32_t len = _p.access_size;
    const uint32_t iters = _p.iterations;
    const bool initiator = _p.initiator;
    const std::string &wait_ev = _p.wait_event_name;
    const std::string &post_ev = _p.post_event_name;

    std::vector<uint8_t> buf(len, 0);

    Tick accumulated = 0;
    uint32_t measured = 0;

    DPRINTF(ChiTestbenchGem5,
            "%s ping_pong: initiator=%s iters=%u wait='%s' post='%s'\n",
            drv.name(), initiator ? "yes" : "no", iters, wait_ev.c_str(),
            post_ev.c_str());

    for (uint32_t i = 0; i < iters; i++) {
        if (!initiator) {
            drv.wait_on(wait_ev);
        }

        // Pattern-unique byte so each write carries real data.
        buf[0] = static_cast<uint8_t>((initiator ? 0xA0u : 0xB0u) + i);

        const Tick t0 = curTick();
        drv.write(_p.line_addr, buf.data(), len);
        const Tick dt = curTick() - t0;

        // Skip iteration 0 for the initiator — the cold transfer is
        // a first-touch miss, not a cache-to-cache round trip.
        if (!(initiator && i == 0)) {
            accumulated += dt;
            measured++;
        }

        drv.notify(post_ev);

        if (initiator) {
            drv.wait_on(wait_ev);
        }
    }

    if (measured > 0) {
        const uint64_t avg = accumulated / measured;
        DPRINTF(ChiTestbenchGem5,
                "%s ping_pong: measured=%u avg_round=%llu ticks\n", drv.name(),
                measured, (unsigned long long)avg);
    }
}

} // namespace chi_gem5tb
} // namespace gem5
