/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "base/trace.hh"
#include "chi_testbench_gem5_v2/driver.hh"
#include "chi_testbench_gem5_v2/sequence_context.hh"
#include "chi_testbench_gem5_v2/sequences/registry.hh"
#include "debug/ChiTestbenchGem5V2.hh"

namespace gem5
{
namespace chi_gem5tb_v2
{
namespace
{

// Two or more tiles each write a distinct byte in the same cache line.
// The CHI protocol still treats this as a coherence conflict — every
// write from one tile forces a snoop of the other tile(s).
void
false_sharing_seq(SequenceContext &ctx)
{
    auto &drv = ctx.drv;
    const auto &p = drv.params();
    const uint64_t addr = p.line_addr + p.byte_offset;
    const uint32_t iters = p.iterations;

    DPRINTF(ChiTestbenchGem5V2, "%s false_sharing: addr=%#llx iters=%u\n",
            drv.name(), (unsigned long long)addr, iters);

    uint8_t byte = 0;
    for (uint32_t i = 0; i < iters; i++) {
        byte = static_cast<uint8_t>(i & 0xFF);
        drv.write_unique_full(addr, &byte, 1);
    }
}

[[maybe_unused]] Registrar _r("false_sharing", &false_sharing_seq);

} // namespace
} // namespace chi_gem5tb_v2
} // namespace gem5
