/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <cstring>
#include <vector>

#include "base/logging.hh"
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

// Scripted CHI walk with data-dependent assertions.
// The driver has no local cache, but the colocated tile does
// (rnf_l2 mode). Writes-then-reads verify that the tile's cache and
// the mesh-wide coherence round-trip preserve data across evictions.
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

    DPRINTF(ChiTestbenchGem5V2,
            "%s opcode_walk: target=%#llx filler=%#llx x %u\n", drv.name(),
            (unsigned long long)target, (unsigned long long)filler_base,
            filler_lines);

    // Step 1: cold ReadShared on the target line.
    drv.read_shared(target, buf.data(), LINE);
    passed++;
    DPRINTF(ChiTestbenchGem5V2,
            "%s opcode_walk: step 1 OK (cold ReadShared)\n", drv.name());

    // Step 2: write a full-line pattern, then read it back.
    const uint8_t pattern = 0xA5;
    std::fill(buf.begin(), buf.end(), pattern);
    drv.write_unique_full(target, buf.data(), LINE);

    std::vector<uint8_t> readback(LINE, 0);
    drv.read_shared(target, readback.data(), LINE);
    for (uint32_t i = 0; i < LINE; i++) {
        if (readback[i] != pattern) {
            panic("%s opcode_walk step 2: byte %u = %#x, expected %#x",
                  drv.name(), i, readback[i], pattern);
        }
    }
    passed++;
    DPRINTF(ChiTestbenchGem5V2,
            "%s opcode_walk: step 2 OK (WriteUniqueFull + hot readback)\n",
            drv.name());

    // Step 3: apply capacity pressure so the target falls out of the
    // tile's cache.
    for (uint32_t i = 0; i < filler_lines; i++) {
        uint8_t tmp[8];
        drv.read_shared(filler_base + (uint64_t)i * LINE, tmp, sizeof(tmp));
    }
    passed++;
    DPRINTF(ChiTestbenchGem5V2,
            "%s opcode_walk: step 3 OK (%u filler reads)\n", drv.name(),
            filler_lines);

    // Step 4: refill from HN; data must still match.
    std::fill(readback.begin(), readback.end(), 0);
    drv.read_shared(target, readback.data(), LINE);
    for (uint32_t i = 0; i < LINE; i++) {
        if (readback[i] != pattern) {
            panic("%s opcode_walk step 4: byte %u = %#x, expected %#x "
                  "(writeback/refill broken?)",
                  drv.name(), i, readback[i], pattern);
        }
    }
    passed++;
    DPRINTF(ChiTestbenchGem5V2,
            "%s opcode_walk: step 4 OK (post-evict refill preserves data). "
            "PASS %u/4\n",
            drv.name(), passed);
}

[[maybe_unused]] Registrar _r("opcode_walk", &opcode_walk_seq);

} // namespace
} // namespace chi_gem5tb_v2
} // namespace gem5
