/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CHI_TESTBENCH_GEM5_SYNC_BARRIER_HH__
#define __CHI_TESTBENCH_GEM5_SYNC_BARRIER_HH__

#include <cstdint>

#include "params/ChiGem5Barrier.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace chi_gem5tb
{

/**
 * Counter-based completion barrier.
 *
 * Drivers that opt in call signal_finish() when their sequence
 * returns. When the call count reaches `expected`, the barrier
 * invokes gem5::exitSimLoop() to terminate the simulation. Overshoot
 * (more signals than expected) is a fatal programming error.
 *
 * Exposed to Python as the ChiBarrier SimObject so scenarios can pass
 * the same barrier reference into each driver's params.
 */
class ChiBarrier : public SimObject
{
  public:
    using Params = ChiGem5BarrierParams;
    explicit ChiBarrier(const Params &p);

    void signal_finish();

  private:
    const uint32_t expected;
    uint32_t count;
};

} // namespace chi_gem5tb
} // namespace gem5

#endif // __CHI_TESTBENCH_GEM5_SYNC_BARRIER_HH__
