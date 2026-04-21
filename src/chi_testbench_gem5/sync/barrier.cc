/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5/sync/barrier.hh"

#include "base/logging.hh"
#include "debug/ChiTestbenchGem5.hh"
#include "sim/sim_exit.hh"

namespace gem5
{
namespace chi_gem5tb
{

ChiBarrier::ChiBarrier(const Params &p)
    : SimObject(p), expected(p.expected), count(0)
{
    if (expected == 0) {
        panic("ChiBarrier %s: expected=0 would never fire", name());
    }
}

void
ChiBarrier::signal_finish()
{
    count++;
    DPRINTF(ChiTestbenchGem5, "ChiBarrier %s: %u/%u\n", name(), count,
            expected);
    if (count == expected) {
        DPRINTF(ChiTestbenchGem5,
                "ChiBarrier %s: all participants done, exiting sim\n", name());
        exitSimLoop("chi_testbench_gem5 finished", 0);
    } else if (count > expected) {
        panic("ChiBarrier %s: signal_finish() called more than expected=%u "
              "times",
              name(), expected);
    }
}

} // namespace chi_gem5tb
} // namespace gem5
