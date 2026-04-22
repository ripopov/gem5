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
smoke_opcode_mix_seq(SequenceContext &ctx)
{
    auto &drv = ctx.drv;
    const auto &p = drv.params();
    const auto &addresses = p.addresses;
    const uint32_t len = p.access_size;
    const uint32_t iters = p.iterations;
    const uint32_t percent_reads = p.percent_reads;

    if (addresses.empty() || iters == 0) {
        return;
    }

    std::vector<uint8_t> buf(len, 0);

    // Deterministic LCG so runs are reproducible under a given seed.
    uint32_t rng = p.seed ? p.seed : 0x12345678u;
    auto next_rand = [&rng]() {
        rng = rng * 1664525u + 1013904223u;
        return rng;
    };

    uint32_t reads = 0, writes = 0;
    for (uint32_t it = 0; it < iters; it++) {
        for (uint64_t addr : addresses) {
            if ((next_rand() % 100) < percent_reads) {
                drv.read(addr, buf.data(), len);
                reads++;
            } else {
                buf[0] = static_cast<uint8_t>(next_rand() & 0xFF);
                drv.write(addr, buf.data(), len);
                writes++;
            }
        }
    }
    DPRINTF(ChiTestbenchGem5, "%s smoke_opcode_mix: %u reads + %u writes\n",
            drv.name(), reads, writes);
}

[[maybe_unused]] Registrar _r("smoke_opcode_mix", &smoke_opcode_mix_seq);

} // namespace
} // namespace chi_gem5tb
} // namespace gem5
