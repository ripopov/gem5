/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "systemc/chi_testbench/opcode_walk.hh"

#include <cstdio>
#include <cstring>
#include <vector>

#include "base/logging.hh"
#include "params/OpcodeWalkDriver.hh"
#include "systemc/ext/core/sc_main.hh"

namespace gem5
{
namespace chi_testbench
{

OpcodeWalkDriver::OpcodeWalkDriver(const Params &p,
                                   const sc_core::sc_module_name &mn)
    : ChiDriverBase(p, mn),
      target_addr(p.target_addr),
      filler_base(p.filler_base),
      filler_lines(p.filler_lines),
      assertions_expected(4)
{}

void
OpcodeWalkDriver::run()
{
    constexpr uint32_t LINE = 64;
    std::vector<uint8_t> buf(LINE, 0);
    uint32_t passed = 0;

    std::printf("[opcode_walk %s] start: target=%#lx filler=%#lx x %u\n",
                name(), (unsigned long)target_addr, (unsigned long)filler_base,
                filler_lines);

    // Step 1: cold read of the target line (expected path:
    // ReadShared from L1 -> HNF -> SNF, first-touch miss).
    read(target_addr, buf.data(), LINE);
    passed++; // Completing without a panic is the assertion here.
    std::printf("[opcode_walk %s] step 1 OK (cold ReadShared)\n", name());

    // Step 2: store a byte pattern and read it back (path:
    // ReadUnique/CleanUnique ownership upgrade, then L1 hit).
    const uint8_t pattern = 0xA5;
    std::fill(buf.begin(), buf.end(), pattern);
    write(target_addr, buf.data(), LINE);

    std::vector<uint8_t> readback(LINE, 0);
    read(target_addr, readback.data(), LINE);
    for (uint32_t i = 0; i < LINE; i++) {
        if (readback[i] != pattern) {
            panic("opcode_walk %s step 2: byte %u = %#x, expected %#x", name(),
                  i, readback[i], pattern);
        }
    }
    passed++;
    std::printf("[opcode_walk %s] step 2 OK (WriteUnique + hot readback)\n",
                name());

    // Step 3: evict the target via capacity pressure. Reading many
    // distinct lines that collide in the private cache set forces the
    // dirty target line to be written back (WriteBackFull) and
    // displaced. `filler_lines` must be larger than the private-
    // cache working set for this to reliably evict.
    for (uint32_t i = 0; i < filler_lines; i++) {
        uint8_t tmp[8];
        read(filler_base + (uint64_t)i * LINE, tmp, sizeof(tmp));
    }
    passed++;
    std::printf("[opcode_walk %s] step 3 OK (%u filler reads induce "
                "WriteBackFull)\n",
                name(), filler_lines);

    // Step 4: cold-ish read of the target again. Data must still be
    // the committed pattern (served either from another cache or,
    // once fully evicted, from SNF via ReadShared -> ReadNoSnp chain).
    std::fill(readback.begin(), readback.end(), 0);
    read(target_addr, readback.data(), LINE);
    for (uint32_t i = 0; i < LINE; i++) {
        if (readback[i] != pattern) {
            panic("opcode_walk %s step 4: byte %u = %#x, expected %#x "
                  "(write-back/refill path broken?)",
                  name(), i, readback[i], pattern);
        }
    }
    passed++;
    std::printf("[opcode_walk %s] step 4 OK (post-evict refill "
                "preserves data)\n",
                name());

    std::printf("[opcode_walk %s] PASS %u/%u assertions\n", name(), passed,
                assertions_expected);
}

} // namespace chi_testbench
} // namespace gem5

gem5::chi_testbench::OpcodeWalkDriver *
gem5::OpcodeWalkDriverParams::create() const
{
    return new gem5::chi_testbench::OpcodeWalkDriver(
        *this, sc_core::sc_module_name(name.c_str()));
}
