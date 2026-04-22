/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5_v2/sync/barrier.hh"

#include "base/logging.hh"
#include "debug/ChiTestbenchGem5V2.hh"
#include "sim/sim_exit.hh"

namespace gem5
{
namespace chi_gem5tb_v2
{

ChiBarrier::ChiBarrier(const Params &p)
    : SimObject(p), expected(p.expected), count(0)
{
    if (expected == 0) {
        panic("ChiGem5V2Barrier %s: expected=0 would never fire", name());
    }
}

void
ChiBarrier::signal_finish()
{
    count++;
    DPRINTF(ChiTestbenchGem5V2, "ChiGem5V2Barrier %s: %u/%u\n", name(), count,
            expected);
    if (count == expected) {
        DPRINTF(ChiTestbenchGem5V2,
                "ChiGem5V2Barrier %s: all participants done, exiting sim\n",
                name());
        exitSimLoop("chi_testbench_gem5_v2 finished", 0);
    } else if (count > expected) {
        panic("ChiGem5V2Barrier %s: signal_finish() called more than "
              "expected=%u times",
              name(), expected);
    }
}

} // namespace chi_gem5tb_v2
} // namespace gem5
