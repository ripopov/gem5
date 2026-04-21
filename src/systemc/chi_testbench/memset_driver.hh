/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SYSTEMC_CHI_TESTBENCH_MEMSET_DRIVER_HH__
#define __SYSTEMC_CHI_TESTBENCH_MEMSET_DRIVER_HH__

#include "params/MemsetDriver.hh"
#include "systemc/chi_testbench/driver_base.hh"

namespace gem5
{
namespace chi_testbench
{

/**
 * Pipelined memset using the non-blocking TLM path. Keeps up to
 * `pipeline_depth` outstanding writes filling a contiguous address
 * range with a fixed byte pattern.
 *
 * Exercises the write-stream path (ReadUnique -> WriteBackFull) under
 * steady offered load.
 */
class MemsetDriver : public ChiDriverBase
{
  public:
    using Params = MemsetDriverParams;
    MemsetDriver(const Params &p, const sc_core::sc_module_name &mn);

  protected:
    void run() override;

  private:
    const uint64_t dst_base;
    const uint32_t num_lines;
    const uint32_t line_size;
    const uint32_t pipeline_depth;
    const uint32_t fill_byte;
};

} // namespace chi_testbench
} // namespace gem5

#endif // __SYSTEMC_CHI_TESTBENCH_MEMSET_DRIVER_HH__
