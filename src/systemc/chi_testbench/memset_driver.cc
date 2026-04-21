/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "systemc/chi_testbench/memset_driver.hh"

#include <cstdio>
#include <cstring>
#include <vector>

#include "base/logging.hh"
#include "params/MemsetDriver.hh"
#include "systemc/ext/core/sc_main.hh"
#include "systemc/ext/core/sc_time.hh"

namespace gem5
{
namespace chi_testbench
{

MemsetDriver::MemsetDriver(const Params &p, const sc_core::sc_module_name &mn)
    : ChiDriverBase(p, mn),
      dst_base(p.dst_base),
      num_lines(p.num_lines),
      line_size(p.line_size),
      pipeline_depth(p.pipeline_depth ? p.pipeline_depth : 1),
      fill_byte(p.fill_byte)
{}

void
MemsetDriver::run()
{
    if (num_lines == 0) {
        std::printf("[memset %s] num_lines=0; nothing to do\n", name());
        return;
    }

    // One payload buffer per pipeline slot; filled with fill_byte.
    std::vector<std::vector<uint8_t>> buffers(
        pipeline_depth,
        std::vector<uint8_t>(line_size, static_cast<uint8_t>(fill_byte)));

    struct Slot
    {
        Handle handle;
        bool busy;
    };
    std::vector<Slot> slots(pipeline_depth, {0, false});

    std::printf("[memset %s] start: num_lines=%u depth=%u "
                "line_size=%u\n",
                name(), num_lines, pipeline_depth, line_size);

    const sc_core::sc_time t_start = sc_core::sc_time_stamp();

    uint32_t next_issue = 0;
    uint32_t retired = 0;

    // Prime the pipeline.
    for (uint32_t s = 0; s < pipeline_depth && next_issue < num_lines; s++) {
        slots[s].handle = async_write(dst_base + next_issue * line_size,
                                      buffers[s].data(), line_size);
        slots[s].busy = true;
        next_issue++;
    }

    while (retired < num_lines) {
        for (uint32_t s = 0; s < pipeline_depth; s++) {
            if (!slots[s].busy) {
                continue;
            }
            resolve(slots[s].handle);
            retired++;
            if (next_issue < num_lines) {
                slots[s].handle =
                    async_write(dst_base + next_issue * line_size,
                                buffers[s].data(), line_size);
                next_issue++;
            } else {
                slots[s].busy = false;
            }
            break;
        }
    }

    const sc_core::sc_time elapsed = sc_core::sc_time_stamp() - t_start;

    const uint64_t bytes = (uint64_t)num_lines * line_size;
    std::printf("[memset %s] done: %u lines (%lu bytes) in %s "
                "depth=%u\n",
                name(), num_lines, (unsigned long)bytes,
                elapsed.to_string().c_str(), pipeline_depth);
}

} // namespace chi_testbench
} // namespace gem5

gem5::chi_testbench::MemsetDriver *
gem5::MemsetDriverParams::create() const
{
    return new gem5::chi_testbench::MemsetDriver(
        *this, sc_core::sc_module_name(name.c_str()));
}
