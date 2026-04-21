/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "systemc/chi_testbench/memcpy_driver.hh"

#include <cstdio>
#include <cstring>
#include <vector>

#include "base/logging.hh"
#include "params/MemcpyDriver.hh"
#include "systemc/ext/core/sc_main.hh"
#include "systemc/ext/core/sc_time.hh"

namespace gem5
{
namespace chi_testbench
{

MemcpyDriver::MemcpyDriver(const Params &p, const sc_core::sc_module_name &mn)
    : ChiDriverBase(p, mn),
      src_base(p.src_base),
      dst_base(p.dst_base),
      num_lines(p.num_lines),
      line_size(p.line_size),
      pipeline_depth(p.pipeline_depth ? p.pipeline_depth : 1)
{}

void
MemcpyDriver::run()
{
    if (num_lines == 0) {
        std::printf("[memcpy %s] num_lines=0; nothing to do\n", name());
        return;
    }

    // Per-slot buffers. Each in-flight read/write points at slot
    // buffers[slot]; when a read completes we immediately fire a write
    // from the same buffer.
    std::vector<std::vector<uint8_t>> buffers(
        pipeline_depth, std::vector<uint8_t>(line_size, 0));

    // For each slot, track which line index it is currently handling
    // and whether we are in the read or write phase.
    struct Slot
    {
        Handle handle;
        uint32_t line_idx;
        enum
        {
            IDLE,
            READING,
            WRITING
        } phase;
    };
    std::vector<Slot> slots(pipeline_depth, {0, 0, Slot::IDLE});

    std::printf("[memcpy %s] start: num_lines=%u depth=%u "
                "line_size=%u\n",
                name(), num_lines, pipeline_depth, line_size);

    const sc_core::sc_time t_start = sc_core::sc_time_stamp();

    // Prime the pipeline with up to `pipeline_depth` reads.
    uint32_t next_issue = 0;
    for (uint32_t s = 0; s < pipeline_depth && next_issue < num_lines; s++) {
        slots[s].handle = async_read(src_base + next_issue * line_size,
                                     buffers[s].data(), line_size);
        slots[s].line_idx = next_issue;
        slots[s].phase = Slot::READING;
        next_issue++;
    }

    uint32_t retired = 0;
    while (retired < num_lines) {
        // Find any in-flight slot and resolve it. We can't use a
        // generic "wait for first completion" primitive without a
        // shared sc_event, so we pick the front slot that is active
        // and resolve it (FIFO-ish but not strictly).
        for (uint32_t s = 0; s < pipeline_depth; s++) {
            if (slots[s].phase == Slot::IDLE) {
                continue;
            }
            resolve(slots[s].handle); // blocks until THIS one retires

            if (slots[s].phase == Slot::READING) {
                // Pair the read with a write of the same buffer.
                slots[s].handle =
                    async_write(dst_base + slots[s].line_idx * line_size,
                                buffers[s].data(), line_size);
                slots[s].phase = Slot::WRITING;
            } else {
                // Write completed; slot is free. Refill from the
                // remaining work list.
                if (next_issue < num_lines) {
                    slots[s].handle =
                        async_read(src_base + next_issue * line_size,
                                   buffers[s].data(), line_size);
                    slots[s].line_idx = next_issue;
                    slots[s].phase = Slot::READING;
                    next_issue++;
                } else {
                    slots[s].phase = Slot::IDLE;
                }
                retired++;
            }
            break; // re-scan from the top each round to keep it
                   // simple and avoid iterator invalidation surprises
        }
    }

    const sc_core::sc_time elapsed = sc_core::sc_time_stamp() - t_start;

    const uint64_t bytes = (uint64_t)num_lines * line_size * 2; // R+W
    std::printf("[memcpy %s] done: %u lines (%lu bytes) in %s "
                "depth=%u\n",
                name(), num_lines, (unsigned long)bytes,
                elapsed.to_string().c_str(), pipeline_depth);
}

} // namespace chi_testbench
} // namespace gem5

gem5::chi_testbench::MemcpyDriver *
gem5::MemcpyDriverParams::create() const
{
    return new gem5::chi_testbench::MemcpyDriver(
        *this, sc_core::sc_module_name(name.c_str()));
}
