/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5/sequences/memset.hh"

#include <vector>

#include "base/cprintf.hh"
#include "base/trace.hh"
#include "chi_testbench_gem5/driver.hh"
#include "debug/ChiTestbenchGem5.hh"

namespace gem5
{
namespace chi_gem5tb
{

void
MemsetSequence::run(ChiSeqDriver &drv)
{
    const uint32_t num_lines = _p.num_lines;
    const uint32_t line_size = _p.line_size;
    const uint32_t depth = _p.pipeline_depth ? _p.pipeline_depth : 1;
    const uint64_t dst_base = _p.dst_base;

    if (num_lines == 0) {
        return;
    }

    std::vector<uint8_t> buffer(line_size, static_cast<uint8_t>(_p.fill_byte));

    DPRINTF(ChiTestbenchGem5,
            "%s memset: num_lines=%u depth=%u line_size=%u\n", drv.name(),
            num_lines, depth, line_size);

    const Tick t_start = curTick();

    uint32_t next_issue = 0;
    uint32_t retired = 0;

    while (retired < num_lines) {
        while (next_issue < num_lines && drv.outstanding() < depth &&
               drv.request_ready()) {
            drv.async_write_req(dst_base + next_issue * line_size,
                                buffer.data(), line_size);
            next_issue++;
        }

        if (auto resp = drv.try_read_resp()) {
            retired++;
            continue;
        }

        if (drv.outstanding() != 0) {
            drv.wait_cycles(Cycles(1));
        }
    }

    const Tick elapsed = curTick() - t_start;
    const uint64_t bytes = (uint64_t)num_lines * line_size;
    cprintf("%s memset: %u lines (%llu bytes) in %llu ticks depth=%u\n",
            drv.name(), num_lines, (unsigned long long)bytes,
            (unsigned long long)elapsed, depth);
}

} // namespace chi_gem5tb
} // namespace gem5
