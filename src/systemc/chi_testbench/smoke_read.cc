/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "systemc/chi_testbench/smoke_read.hh"

#include <cstdio>
#include <cstring>
#include <vector>

#include "params/SmokeReadDriver.hh"
#include "systemc/ext/core/sc_main.hh"
#include "systemc/ext/core/sc_time.hh"

namespace gem5
{
namespace chi_testbench
{

SmokeReadDriver::SmokeReadDriver(const Params &p,
                                 const sc_core::sc_module_name &mn)
    : ChiDriverBase(p, mn),
      addresses(p.addresses.begin(), p.addresses.end()),
      access_size(p.access_size),
      iterations(p.iterations),
      stop_on_finish(p.stop_on_finish)
{}

void
SmokeReadDriver::run()
{
    if (addresses.empty()) {
        std::printf("[smoke_read %s] no addresses; exiting\n", name());
        if (stop_on_finish) {
            sc_core::sc_stop();
        }
        return;
    }

    std::vector<uint8_t> buf(access_size, 0);

    std::printf("[smoke_read %s] starting: %zu addresses x %u iters "
                "x %u bytes\n",
                name(), addresses.size(), iterations, access_size);

    const sc_core::sc_time t_start = sc_core::sc_time_stamp();

    for (uint32_t it = 0; it < iterations; it++) {
        for (uint64_t addr : addresses) {
            read(addr, buf.data(), access_size);
        }
    }

    const sc_core::sc_time elapsed = sc_core::sc_time_stamp() - t_start;

    std::printf("[smoke_read %s] done: %u accesses in %s\n", name(),
                iterations * (uint32_t)addresses.size(),
                elapsed.to_string().c_str());

    if (stop_on_finish) {
        sc_core::sc_stop();
    }
}

} // namespace chi_testbench
} // namespace gem5

gem5::chi_testbench::SmokeReadDriver *
gem5::SmokeReadDriverParams::create() const
{
    return new gem5::chi_testbench::SmokeReadDriver(
        *this, sc_core::sc_module_name(name.c_str()));
}
