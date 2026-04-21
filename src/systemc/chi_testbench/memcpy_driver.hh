/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SYSTEMC_CHI_TESTBENCH_MEMCPY_DRIVER_HH__
#define __SYSTEMC_CHI_TESTBENCH_MEMCPY_DRIVER_HH__

#include "params/MemcpyDriver.hh"
#include "systemc/chi_testbench/driver_base.hh"

namespace gem5
{
namespace chi_testbench
{

/**
 * Pipelined memcpy using the non-blocking TLM path. Keeps up to
 * `pipeline_depth` outstanding reads; as each read retires, it issues
 * a matching write. This exercises the mesh under a bandwidth-shaped
 * workload, not a latency-shaped one.
 *
 * Used for sweeping the bandwidth-vs-depth curve.
 */
class MemcpyDriver : public ChiDriverBase
{
  public:
    using Params = MemcpyDriverParams;
    MemcpyDriver(const Params &p, const sc_core::sc_module_name &mn);

  protected:
    void run() override;

  private:
    const uint64_t src_base;
    const uint64_t dst_base;
    const uint32_t num_lines;
    const uint32_t line_size;
    const uint32_t pipeline_depth;
};

} // namespace chi_testbench
} // namespace gem5

#endif // __SYSTEMC_CHI_TESTBENCH_MEMCPY_DRIVER_HH__
