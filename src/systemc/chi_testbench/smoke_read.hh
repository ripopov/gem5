/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SYSTEMC_CHI_TESTBENCH_SMOKE_READ_HH__
#define __SYSTEMC_CHI_TESTBENCH_SMOKE_READ_HH__

#include <vector>

#include "params/SmokeReadDriver.hh"
#include "systemc/chi_testbench/driver_base.hh"

namespace gem5
{
namespace chi_testbench
{

/**
 * Simplest Stage-1 smoke driver. Sequential blocking reads over an
 * author-supplied address list, with a chosen data length per access.
 * Exits the SystemC kernel when the list is exhausted.
 */
class SmokeReadDriver : public ChiDriverBase
{
  public:
    using Params = SmokeReadDriverParams;
    SmokeReadDriver(const Params &p, const sc_core::sc_module_name &mn);

  protected:
    void run() override;

  private:
    const std::vector<uint64_t> addresses;
    const uint32_t access_size;
    const uint32_t iterations;
    const bool stop_on_finish;
};

} // namespace chi_testbench
} // namespace gem5

#endif // __SYSTEMC_CHI_TESTBENCH_SMOKE_READ_HH__
