/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5/sequences/false_sharing.hh"

#include "base/trace.hh"
#include "chi_testbench_gem5/driver.hh"
#include "debug/ChiTestbenchGem5.hh"

namespace gem5
{
namespace chi_gem5tb
{

void
FalseSharingSequence::run(ChiSeqDriver &drv)
{
    const uint64_t addr = _p.line_addr + _p.byte_offset;
    const uint32_t iters = _p.iterations;

    DPRINTF(ChiTestbenchGem5, "%s false_sharing: addr=%#llx iters=%u\n",
            drv.name(), (unsigned long long)addr, iters);

    const Tick t_start = curTick();
    uint8_t byte = 0;
    for (uint32_t i = 0; i < iters; i++) {
        byte = static_cast<uint8_t>(i & 0xFF);
        drv.write(addr, &byte, 1);
    }
    DPRINTF(ChiTestbenchGem5, "%s false_sharing: %u writes in %llu ticks\n",
            drv.name(), iters, (unsigned long long)(curTick() - t_start));
}

} // namespace chi_gem5tb
} // namespace gem5
