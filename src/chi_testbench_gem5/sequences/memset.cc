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
memset_seq(SequenceContext &ctx)
{
    auto &drv = ctx.drv;
    const auto &p = drv.params();
    const uint32_t num_lines = p.num_lines;
    const uint32_t line_size = p.line_size;
    const uint32_t depth = p.pipeline_depth ? p.pipeline_depth : 1;
    const uint64_t dst_base = p.dst_base;

    if (num_lines == 0) {
        return;
    }

    std::vector<std::vector<uint8_t>> buffers(
        depth,
        std::vector<uint8_t>(line_size, static_cast<uint8_t>(p.fill_byte)));

    struct Slot
    {
        ChiSeqDriver::Handle handle;
        bool busy;
    };
    std::vector<Slot> slots(depth, {0, false});

    DPRINTF(ChiTestbenchGem5,
            "%s memset: num_lines=%u depth=%u line_size=%u\n", drv.name(),
            num_lines, depth, line_size);

    const Tick t_start = curTick();

    uint32_t next_issue = 0, retired = 0;
    for (uint32_t s = 0; s < depth && next_issue < num_lines; s++) {
        slots[s].handle = drv.async_write(dst_base + next_issue * line_size,
                                          buffers[s].data(), line_size);
        slots[s].busy = true;
        next_issue++;
    }

    while (retired < num_lines) {
        for (uint32_t s = 0; s < depth; s++) {
            if (!slots[s].busy) {
                continue;
            }
            drv.resolve(slots[s].handle);
            retired++;
            if (next_issue < num_lines) {
                slots[s].handle =
                    drv.async_write(dst_base + next_issue * line_size,
                                    buffers[s].data(), line_size);
                next_issue++;
            } else {
                slots[s].busy = false;
            }
            break;
        }
    }

    const Tick elapsed = curTick() - t_start;
    const uint64_t bytes = (uint64_t)num_lines * line_size;
    DPRINTF(ChiTestbenchGem5,
            "%s memset: %u lines (%llu bytes) in %llu ticks depth=%u\n",
            drv.name(), num_lines, (unsigned long long)bytes,
            (unsigned long long)elapsed, depth);
}

[[maybe_unused]] Registrar _r("memset", &memset_seq);

} // namespace
} // namespace chi_gem5tb
} // namespace gem5
