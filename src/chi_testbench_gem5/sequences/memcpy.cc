/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

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

// Pipelined memcpy — K slots, each cycling READING→WRITING→free.
// Up to pipeline_depth LD+ST pairs in flight at any moment.
void
memcpy_seq(SequenceContext &ctx)
{
    auto &drv = ctx.drv;
    const auto &p = drv.params();
    const uint32_t num_lines = p.num_lines;
    const uint32_t line_size = p.line_size;
    const uint32_t depth = p.pipeline_depth ? p.pipeline_depth : 1;
    const uint64_t src_base = p.src_base;
    const uint64_t dst_base = p.dst_base;

    if (num_lines == 0) {
        return;
    }

    std::vector<std::vector<uint8_t>> buffers(
        depth, std::vector<uint8_t>(line_size, 0));

    struct Slot
    {
        ChiSeqDriver::Handle handle;
        uint32_t line_idx;
        enum
        {
            IDLE,
            READING,
            WRITING
        } phase;
    };
    std::vector<Slot> slots(depth, {0, 0, Slot::IDLE});

    DPRINTF(ChiTestbenchGem5,
            "%s memcpy: num_lines=%u depth=%u line_size=%u\n", drv.name(),
            num_lines, depth, line_size);

    const Tick t_start = curTick();

    uint32_t next_issue = 0;
    for (uint32_t s = 0; s < depth && next_issue < num_lines; s++) {
        slots[s].handle = drv.async_read(src_base + next_issue * line_size,
                                         buffers[s].data(), line_size);
        slots[s].line_idx = next_issue;
        slots[s].phase = Slot::READING;
        next_issue++;
    }

    uint32_t retired = 0;
    while (retired < num_lines) {
        for (uint32_t s = 0; s < depth; s++) {
            if (slots[s].phase == Slot::IDLE) {
                continue;
            }
            drv.resolve(slots[s].handle);
            if (slots[s].phase == Slot::READING) {
                slots[s].handle =
                    drv.async_write(dst_base + slots[s].line_idx * line_size,
                                    buffers[s].data(), line_size);
                slots[s].phase = Slot::WRITING;
            } else {
                if (next_issue < num_lines) {
                    slots[s].handle =
                        drv.async_read(src_base + next_issue * line_size,
                                       buffers[s].data(), line_size);
                    slots[s].line_idx = next_issue;
                    slots[s].phase = Slot::READING;
                    next_issue++;
                } else {
                    slots[s].phase = Slot::IDLE;
                }
                retired++;
            }
            break;
        }
    }

    const Tick elapsed = curTick() - t_start;
    const uint64_t bytes = (uint64_t)num_lines * line_size * 2;
    DPRINTF(ChiTestbenchGem5,
            "%s memcpy: %u lines (%llu bytes) in %llu ticks depth=%u\n",
            drv.name(), num_lines, (unsigned long long)bytes,
            (unsigned long long)elapsed, depth);
}

[[maybe_unused]] Registrar _r("memcpy", &memcpy_seq);

} // namespace
} // namespace chi_gem5tb
} // namespace gem5
