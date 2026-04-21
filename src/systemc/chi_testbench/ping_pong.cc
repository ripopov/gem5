/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "systemc/chi_testbench/ping_pong.hh"

#include <cstdio>
#include <vector>

#include "base/logging.hh"
#include "params/PingPongDriver.hh"
#include "systemc/ext/core/sc_main.hh"
#include "systemc/ext/core/sc_time.hh"

namespace gem5
{
namespace chi_testbench
{

PingPongDriver::PingPongDriver(const Params &p,
                               const sc_core::sc_module_name &mn)
    : ChiDriverBase(p, mn),
      line_addr(p.line_addr),
      access_size(p.access_size),
      iterations(p.iterations),
      initiator(p.initiator),
      wait_event(p.wait_event),
      post_event(p.post_event),
      bus(p.bus)
{
    if (!bus) {
        panic("PingPongDriver %s requires a ChiEventBus (bus param)", name());
    }
}

void
PingPongDriver::run()
{
    std::vector<uint8_t> buf(access_size, 0);

    // Latency accumulator across measured iterations (skip the first
    // cold iteration so the mean reflects true cache-to-cache transfer
    // cost, not first-touch miss-to-DRAM).
    sc_core::sc_time accumulated = sc_core::SC_ZERO_TIME;
    uint32_t measured = 0;

    std::printf("[ping_pong %s] start: initiator=%s iterations=%u\n", name(),
                initiator ? "yes" : "no", iterations);

    for (uint32_t i = 0; i < iterations; i++) {
        if (!initiator) {
            bus->wait_on(wait_event);
        }

        // Pattern-unique byte so each write carries real data.
        buf[0] = static_cast<uint8_t>((initiator ? 0xA0u : 0xB0u) + i);

        const sc_core::sc_time t0 = sc_core::sc_time_stamp();
        write(line_addr, buf.data(), access_size);
        const sc_core::sc_time dt = sc_core::sc_time_stamp() - t0;

        // Skip iteration 0 for the initiator (cold transfer).
        if (!(initiator && i == 0)) {
            accumulated += dt;
            measured++;
        }

        bus->notify(post_event);

        if (initiator) {
            bus->wait_on(wait_event);
        }
    }

    if (measured > 0) {
        // Compute an integer average in picoseconds to avoid pulling
        // in double arithmetic for a fast path.
        const uint64_t total_ps = accumulated.value();
        const uint64_t avg_ps = total_ps / measured;
        std::printf("[ping_pong %s] done: measured=%u avg_round=%lu ps\n",
                    name(), measured, (unsigned long)avg_ps);
    } else {
        std::printf("[ping_pong %s] done: no measured iterations\n", name());
    }
}

} // namespace chi_testbench
} // namespace gem5

gem5::chi_testbench::PingPongDriver *
gem5::PingPongDriverParams::create() const
{
    return new gem5::chi_testbench::PingPongDriver(
        *this, sc_core::sc_module_name(name.c_str()));
}
