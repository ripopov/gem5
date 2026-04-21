/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <vector>

#include "base/trace.hh"
#include "chi_testbench_gem5/driver.hh"
#include "chi_testbench_gem5/sequence_context.hh"
#include "chi_testbench_gem5/sequences/registry.hh"
#include "debug/ChiTestbenchGem5.hh"

namespace gem5
{
namespace chi_gem5tb
{
namespace
{

void
smoke_write_seq(SequenceContext &ctx)
{
    auto &drv = ctx.drv;
    const auto &p = drv.params();
    const auto &addresses = p.addresses;
    const uint32_t len = p.access_size;
    const uint32_t iters = p.iterations;

    if (addresses.empty()) {
        return;
    }

    std::vector<uint8_t> buf(len, 0);
    DPRINTF(ChiTestbenchGem5,
            "%s smoke_write: %zu addrs x %u iters x %u bytes\n", drv.name(),
            addresses.size(), iters, len);

    const Tick t_start = curTick();
    for (uint32_t it = 0; it < iters; it++) {
        for (uint64_t addr : addresses) {
            // Unique byte per iteration so stores carry non-trivial
            // data (matches the SystemC smoke_write pattern).
            buf[0] = static_cast<uint8_t>(it & 0xFF);
            drv.write(addr, buf.data(), len);
        }
    }
    DPRINTF(ChiTestbenchGem5, "%s smoke_write: %u accesses in %llu ticks\n",
            drv.name(), iters * (uint32_t)addresses.size(),
            (unsigned long long)(curTick() - t_start));
}

[[maybe_unused]] Registrar _r("smoke_write", &smoke_write_seq);

} // namespace
} // namespace chi_gem5tb
} // namespace gem5
