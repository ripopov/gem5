/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SYSTEMC_CHI_TESTBENCH_FALSE_SHARING_HH__
#define __SYSTEMC_CHI_TESTBENCH_FALSE_SHARING_HH__

#include "params/FalseSharingDriver.hh"
#include "systemc/chi_testbench/driver_base.hh"

namespace gem5
{
namespace chi_testbench
{

/**
 * Writes one byte repeatedly at a fixed offset inside a cache line.
 * Two or more of these drivers instantiated on different tiles, each
 * with a different `byte_offset` inside the same `line_addr`, induce
 * invalidation storms on the SNP vnet while making zero real data
 * dependency on each other — the textbook false-sharing pattern.
 */
class FalseSharingDriver : public ChiDriverBase
{
  public:
    using Params = FalseSharingDriverParams;
    FalseSharingDriver(const Params &p, const sc_core::sc_module_name &mn);

  protected:
    void run() override;

  private:
    const uint64_t line_addr;
    const uint32_t byte_offset;
    const uint32_t iterations;
};

} // namespace chi_testbench
} // namespace gem5

#endif // __SYSTEMC_CHI_TESTBENCH_FALSE_SHARING_HH__
