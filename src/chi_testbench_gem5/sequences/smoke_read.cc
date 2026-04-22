/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5/sequences/smoke_read.hh"

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
SmokeReadSequence::run(ChiSeqDriver &drv)
{
    const auto &addresses = _p.addresses;
    const uint32_t len = _p.access_size;
    const uint32_t iters = _p.iterations;

    if (addresses.empty()) {
        DPRINTF(ChiTestbenchGem5, "%s: no addresses, exiting\n", drv.name());
        return;
    }

    std::vector<uint8_t> buf(len, 0);
    DPRINTF(ChiTestbenchGem5,
            "%s smoke_read: %zu addrs x %u iters x %u bytes\n", drv.name(),
            addresses.size(), iters, len);

    const Tick t_start = curTick();
    for (uint32_t it = 0; it < iters; it++) {
        for (uint64_t addr : addresses) {
            drv.read(addr, buf.data(), len);
        }
    }
    DPRINTF(ChiTestbenchGem5, "%s smoke_read: %u accesses in %llu ticks\n",
            drv.name(), iters * (uint32_t)addresses.size(),
            (unsigned long long)(curTick() - t_start));
}

} // namespace chi_gem5tb
} // namespace gem5
