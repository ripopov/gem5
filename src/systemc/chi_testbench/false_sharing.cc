/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "systemc/chi_testbench/false_sharing.hh"

#include <cstdio>

#include "params/FalseSharingDriver.hh"
#include "systemc/ext/core/sc_main.hh"
#include "systemc/ext/core/sc_time.hh"

namespace gem5
{
namespace chi_testbench
{

FalseSharingDriver::FalseSharingDriver(const Params &p,
                                       const sc_core::sc_module_name &mn)
    : ChiDriverBase(p, mn),
      line_addr(p.line_addr),
      byte_offset(p.byte_offset),
      iterations(p.iterations)
{}

void
FalseSharingDriver::run()
{
    const uint64_t addr = line_addr + byte_offset;
    uint8_t byte = 0;

    std::printf("[false_sharing %s] start: addr=%#lx iterations=%u\n", name(),
                (unsigned long)addr, iterations);

    const sc_core::sc_time t_start = sc_core::sc_time_stamp();

    for (uint32_t i = 0; i < iterations; i++) {
        byte = static_cast<uint8_t>(i & 0xFF);
        write(addr, &byte, 1);
    }

    const sc_core::sc_time elapsed = sc_core::sc_time_stamp() - t_start;

    std::printf("[false_sharing %s] done: %u writes in %s\n", name(),
                iterations, elapsed.to_string().c_str());
}

} // namespace chi_testbench
} // namespace gem5

gem5::chi_testbench::FalseSharingDriver *
gem5::FalseSharingDriverParams::create() const
{
    return new gem5::chi_testbench::FalseSharingDriver(
        *this, sc_core::sc_module_name(name.c_str()));
}
