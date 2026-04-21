/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <cstring>
#include <vector>

#include "base/logging.hh"
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

// Scripted LD/ST walk with data-dependent assertions. Mirrors the
// SystemC OpcodeWalkDriver exactly.
void
opcode_walk_seq(SequenceContext &ctx)
{
    auto &drv = ctx.drv;
    const auto &p = drv.params();
    const uint64_t target = p.target_addr;
    const uint64_t filler_base = p.filler_base;
    const uint32_t filler_lines = p.filler_lines;
    constexpr uint32_t LINE = 64;

    std::vector<uint8_t> buf(LINE, 0);
    uint32_t passed = 0;

    DPRINTF(ChiTestbenchGem5,
            "%s opcode_walk: target=%#llx filler=%#llx x %u\n", drv.name(),
            (unsigned long long)target, (unsigned long long)filler_base,
            filler_lines);

    // Step 1: cold read of the target line.
    drv.read(target, buf.data(), LINE);
    passed++;
    DPRINTF(ChiTestbenchGem5, "%s opcode_walk: step 1 OK (cold ReadShared)\n",
            drv.name());

    // Step 2: store a byte pattern and read it back.
    const uint8_t pattern = 0xA5;
    std::fill(buf.begin(), buf.end(), pattern);
    drv.write(target, buf.data(), LINE);

    std::vector<uint8_t> readback(LINE, 0);
    drv.read(target, readback.data(), LINE);
    for (uint32_t i = 0; i < LINE; i++) {
        if (readback[i] != pattern) {
            panic("%s opcode_walk step 2: byte %u = %#x, expected %#x",
                  drv.name(), i, readback[i], pattern);
        }
    }
    passed++;
    DPRINTF(ChiTestbenchGem5,
            "%s opcode_walk: step 2 OK (WriteUnique + hot readback)\n",
            drv.name());

    // Step 3: evict the target via capacity pressure.
    for (uint32_t i = 0; i < filler_lines; i++) {
        uint8_t tmp[8];
        drv.read(filler_base + (uint64_t)i * LINE, tmp, sizeof(tmp));
    }
    passed++;
    DPRINTF(ChiTestbenchGem5, "%s opcode_walk: step 3 OK (%u filler reads)\n",
            drv.name(), filler_lines);

    // Step 4: cold-ish read of the target; data must still match.
    std::fill(readback.begin(), readback.end(), 0);
    drv.read(target, readback.data(), LINE);
    for (uint32_t i = 0; i < LINE; i++) {
        if (readback[i] != pattern) {
            panic("%s opcode_walk step 4: byte %u = %#x, expected %#x "
                  "(writeback/refill broken?)",
                  drv.name(), i, readback[i], pattern);
        }
    }
    passed++;
    DPRINTF(ChiTestbenchGem5,
            "%s opcode_walk: step 4 OK (post-evict refill preserves data). "
            "PASS %u/4\n",
            drv.name(), passed);
}

[[maybe_unused]] Registrar _r("opcode_walk", &opcode_walk_seq);

} // namespace
} // namespace chi_gem5tb
} // namespace gem5
