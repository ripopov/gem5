/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SYSTEMC_CHI_TESTBENCH_IDLE_DRIVER_HH__
#define __SYSTEMC_CHI_TESTBENCH_IDLE_DRIVER_HH__

#include "params/IdleDriver.hh"
#include "systemc/chi_testbench/driver_base.hh"

namespace gem5
{
namespace chi_testbench
{

/**
 * Does nothing. Mount at tiles that should not generate traffic in a
 * given scenario. Silent (no log output) so 15-of-16 idle tiles do
 * not flood the terminal.
 */
class IdleDriver : public ChiDriverBase
{
  public:
    using Params = IdleDriverParams;
    IdleDriver(const Params &p, const sc_core::sc_module_name &mn);

  protected:
    void run() override;
};

} // namespace chi_testbench
} // namespace gem5

#endif // __SYSTEMC_CHI_TESTBENCH_IDLE_DRIVER_HH__
