/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "systemc/chi_testbench/smoke_opcode_mix.hh"

#include <cstdio>
#include <vector>

#include "params/SmokeOpcodeMixDriver.hh"
#include "systemc/ext/core/sc_main.hh"
#include "systemc/ext/core/sc_time.hh"

namespace gem5
{
namespace chi_testbench
{

SmokeOpcodeMixDriver::SmokeOpcodeMixDriver(const Params &p,
                                           const sc_core::sc_module_name &mn)
    : ChiDriverBase(p, mn),
      addresses(p.addresses.begin(), p.addresses.end()),
      access_size(p.access_size),
      iterations(p.iterations),
      percent_reads(p.percent_reads),
      seed(p.seed),
      stop_on_finish(p.stop_on_finish)
{}

void
SmokeOpcodeMixDriver::run()
{
    if (addresses.empty() || iterations == 0) {
        if (stop_on_finish) {
            sc_core::sc_stop();
        }
        return;
    }

    std::vector<uint8_t> buf(access_size, 0);

    // Standard numerical-recipes LCG — good enough for a smoke test.
    uint32_t rng = seed ? seed : 0x12345678u;
    auto next_rand = [&rng]() -> uint32_t {
        rng = rng * 1664525u + 1013904223u;
        return rng;
    };

    const uint32_t total = iterations * (uint32_t)addresses.size();
    uint32_t n_reads = 0;
    uint32_t n_writes = 0;

    for (uint32_t it = 0; it < iterations; it++) {
        for (uint64_t addr : addresses) {
            const uint32_t r = next_rand() % 100;
            if (r < percent_reads) {
                read(addr, buf.data(), access_size);
                n_reads++;
            } else {
                buf[0] = static_cast<uint8_t>(next_rand() & 0xFF);
                write(addr, buf.data(), access_size);
                n_writes++;
            }
        }
    }

    std::printf("[smoke_opcode_mix %s] done: %u reads + %u writes "
                "(total %u)\n",
                name(), n_reads, n_writes, total);

    if (stop_on_finish) {
        sc_core::sc_stop();
    }
}

} // namespace chi_testbench
} // namespace gem5

gem5::chi_testbench::SmokeOpcodeMixDriver *
gem5::SmokeOpcodeMixDriverParams::create() const
{
    return new gem5::chi_testbench::SmokeOpcodeMixDriver(
        *this, sc_core::sc_module_name(name.c_str()));
}
