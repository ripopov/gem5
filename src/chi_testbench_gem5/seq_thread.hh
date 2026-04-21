/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CHI_TESTBENCH_GEM5_SEQ_THREAD_HH__
#define __CHI_TESTBENCH_GEM5_SEQ_THREAD_HH__

#include "base/fiber.hh"

namespace gem5
{
namespace chi_gem5tb
{

class ChiSeqDriver;

/**
 * Stack-switching fiber that runs one registered test sequence.
 *
 * The driver creates one SeqThread per tile. When the driver's
 * startup() fires, it schedules a zero-delay event that calls
 * seq_thread->run(), which resumes the fiber's main(). The body of
 * main() looks up the registered sequence function by name and
 * invokes it; inside that function the sequence author calls the
 * driver's read()/write()/resolve() helpers, each of which
 * suspends the fiber via yield_to_primary() until the matching
 * event callback resumes it.
 *
 * This is the exact same stack-switching primitive that backs
 * SystemC's SC_THREAD suspension in gem5's SystemC integration; we
 * just expose it directly instead of going through a SystemC kernel.
 */
class SeqThread : public Fiber
{
  public:
    explicit SeqThread(ChiSeqDriver &d, size_t stack_size = DefaultStackSize)
        : Fiber(stack_size), _drv(d)
    {}

    /**
     * Suspend this fiber and return control to gem5's main event
     * loop. Called from the sequence body whenever it hits a wait
     * point (pending response, scheduled wake, latch wait).
     */
    void
    yield_to_primary()
    {
        Fiber::primaryFiber()->run();
    }

    ChiSeqDriver &
    drv()
    {
        return _drv;
    }

  protected:
    void main() override;

  private:
    ChiSeqDriver &_drv;
};

} // namespace chi_gem5tb
} // namespace gem5

#endif // __CHI_TESTBENCH_GEM5_SEQ_THREAD_HH__
