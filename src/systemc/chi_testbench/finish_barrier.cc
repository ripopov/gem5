/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "systemc/chi_testbench/finish_barrier.hh"

#include <cstdio>

#include "base/logging.hh"
#include "systemc/ext/core/sc_main.hh"

namespace gem5
{
namespace chi_testbench
{

ChiFinishBarrier::ChiFinishBarrier(const Params &p,
                                   const sc_core::sc_module_name &mn)
    : sc_core::sc_module(mn), expected(p.expected), count(0)
{
    if (expected == 0) {
        panic("ChiFinishBarrier %s: expected=0 would never fire", name());
    }
}

void
ChiFinishBarrier::signal_finish()
{
    count++;
    if (count == expected) {
        std::printf("[chi_finish_barrier %s] all %u participants done; "
                    "calling sc_stop\n",
                    name(), expected);
        sc_core::sc_stop();
    } else if (count > expected) {
        panic("ChiFinishBarrier %s: signal_finish() called more than "
              "expected=%u times",
              name(), expected);
    }
}

} // namespace chi_testbench
} // namespace gem5

gem5::chi_testbench::ChiFinishBarrier *
gem5::ChiFinishBarrierParams::create() const
{
    return new gem5::chi_testbench::ChiFinishBarrier(
        *this, sc_core::sc_module_name(name.c_str()));
}
