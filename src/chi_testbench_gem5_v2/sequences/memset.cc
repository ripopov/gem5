/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <vector>

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

// Pipelined memset — K slots, each issuing an async WriteUniqueFull and
// recycling on completion.
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
        ChiDriverNode::Handle handle;
        bool busy;
    };
    std::vector<Slot> slots(depth, {0, false});

    DPRINTF(ChiTestbenchGem5V2,
            "%s memset: num_lines=%u depth=%u line_size=%u\n", drv.name(),
            num_lines, depth, line_size);

    uint32_t next_issue = 0, retired = 0;
    for (uint32_t s = 0; s < depth && next_issue < num_lines; s++) {
        slots[s].handle = drv.async_write_unique_full(
            dst_base + next_issue * line_size, buffers[s].data(), line_size);
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
                slots[s].handle = drv.async_write_unique_full(
                    dst_base + next_issue * line_size, buffers[s].data(),
                    line_size);
                next_issue++;
            } else {
                slots[s].busy = false;
            }
            break;
        }
    }
}

[[maybe_unused]] Registrar _r("memset", &memset_seq);

} // namespace
} // namespace chi_gem5tb_v2
} // namespace gem5
