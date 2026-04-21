/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SYSTEMC_CHI_TESTBENCH_SMOKE_WRITE_HH__
#define __SYSTEMC_CHI_TESTBENCH_SMOKE_WRITE_HH__

#include <vector>

#include "params/SmokeWriteDriver.hh"
#include "systemc/chi_testbench/driver_base.hh"

namespace gem5
{
namespace chi_testbench
{

/**
 * Mirror of SmokeReadDriver but issues blocking writes instead of
 * reads. Exercises the ReadUnique -> WriteBackFull path at every HNF
 * slice touched.
 */
class SmokeWriteDriver : public ChiDriverBase
{
  public:
    using Params = SmokeWriteDriverParams;
    SmokeWriteDriver(const Params &p, const sc_core::sc_module_name &mn);

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

#endif // __SYSTEMC_CHI_TESTBENCH_SMOKE_WRITE_HH__
