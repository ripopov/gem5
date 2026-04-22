/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CHI_TESTBENCH_GEM5_V2_SYNC_BARRIER_HH__
#define __CHI_TESTBENCH_GEM5_V2_SYNC_BARRIER_HH__

#include <cstdint>

#include "params/ChiGem5V2Barrier.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace chi_gem5tb_v2
{

/**
 * Counter-based completion barrier. When signal_finish() has been
 * called `expected` times, exits the simulation via exitSimLoop().
 * Overshoot is a fatal programming error.
 */
class ChiBarrier : public SimObject
{
  public:
    using Params = ChiGem5V2BarrierParams;
    explicit ChiBarrier(const Params &p);

    void signal_finish();

  private:
    const uint32_t expected;
    uint32_t count;
};

} // namespace chi_gem5tb_v2
} // namespace gem5

#endif // __CHI_TESTBENCH_GEM5_V2_SYNC_BARRIER_HH__
