/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SYSTEMC_CHI_TESTBENCH_FINISH_BARRIER_HH__
#define __SYSTEMC_CHI_TESTBENCH_FINISH_BARRIER_HH__

#include "params/ChiFinishBarrier.hh"
#include "systemc/ext/core/sc_module.hh"
#include "systemc/ext/core/sc_module_name.hh"

namespace gem5
{
namespace chi_testbench
{

/**
 * Counter-based completion barrier. Each driver that wants to
 * participate calls `signal_finish()` when its run() is about to
 * return; when the call count reaches `expected`, the barrier calls
 * sc_stop() to end the simulation.
 *
 * This is the standard way to coordinate "stop after all tiles have
 * finished their work" when multiple drivers issue traffic
 * concurrently.
 */
class ChiFinishBarrier : public sc_core::sc_module
{
  public:
    using Params = ChiFinishBarrierParams;
    ChiFinishBarrier(const Params &p, const sc_core::sc_module_name &mn);

    void signal_finish();

  private:
    const uint32_t expected;
    uint32_t count;
};

} // namespace chi_testbench
} // namespace gem5

#endif // __SYSTEMC_CHI_TESTBENCH_FINISH_BARRIER_HH__
