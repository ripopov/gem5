/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "systemc/chi_testbench/idle_driver.hh"

namespace gem5
{
namespace chi_testbench
{

IdleDriver::IdleDriver(const Params &p, const sc_core::sc_module_name &mn)
    : ChiDriverBase(p, mn)
{}

void
IdleDriver::run()
{
    // Intentionally empty. The SC_THREAD just returns; the SystemC
    // kernel keeps running as long as some other driver is active.
}

} // namespace chi_testbench
} // namespace gem5

gem5::chi_testbench::IdleDriver *
gem5::IdleDriverParams::create() const
{
    return new gem5::chi_testbench::IdleDriver(
        *this, sc_core::sc_module_name(name.c_str()));
}
