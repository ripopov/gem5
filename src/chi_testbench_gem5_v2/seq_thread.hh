/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CHI_TESTBENCH_GEM5_V2_SEQ_THREAD_HH__
#define __CHI_TESTBENCH_GEM5_V2_SEQ_THREAD_HH__

#include "base/fiber.hh"

namespace gem5
{
namespace chi_gem5tb_v2
{

class ChiDriverNode;

/**
 * Stack-switching fiber that runs one registered test sequence.
 *
 * Each driver owns one SeqThread. On startup the driver schedules a
 * zero-delay event that calls `seq_thread.run()`, which enters
 * main(); the sequence body invokes the driver's CHI API helpers
 * (read_shared, write_back_full, etc.), each of which suspends the
 * fiber via `yield_to_primary()` until the matching CHI response
 * arrives on an inbound MessageBuffer and the driver resumes the
 * fiber.
 *
 * Stack-switching is provided by `gem5::Fiber` (src/base/fiber.hh).
 */
class SeqThread : public Fiber
{
  public:
    explicit SeqThread(ChiDriverNode &d, size_t stack_size = DefaultStackSize)
        : Fiber(stack_size), _drv(d)
    {}

    void
    yield_to_primary()
    {
        Fiber::primaryFiber()->run();
    }

    ChiDriverNode &
    drv()
    {
        return _drv;
    }

  protected:
    void main() override;

  private:
    ChiDriverNode &_drv;
};

} // namespace chi_gem5tb_v2
} // namespace gem5

#endif // __CHI_TESTBENCH_GEM5_V2_SEQ_THREAD_HH__
