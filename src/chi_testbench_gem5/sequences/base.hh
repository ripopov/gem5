/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CHI_TESTBENCH_GEM5_SEQUENCES_BASE_HH__
#define __CHI_TESTBENCH_GEM5_SEQUENCES_BASE_HH__

#include "params/ChiSequence.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace chi_gem5tb
{

class ChiSeqDriver;

/**
 * Abstract Strategy SimObject. Each concrete subclass owns its own
 * strongly-typed Params struct (declared in the per-sequence Python
 * class) and overrides run() with the sequence body.
 *
 * The driver invokes sequence->run(*this) once per fiber kickoff;
 * the sequence calls back into the driver for read/write/wait/notify.
 */
class ChiSequence : public SimObject
{
  public:
    using Params = ChiSequenceParams;
    explicit ChiSequence(const Params &p) : SimObject(p) {}
    ~ChiSequence() override = default;

    virtual void run(ChiSeqDriver &drv) = 0;
};

} // namespace chi_gem5tb
} // namespace gem5

#endif // __CHI_TESTBENCH_GEM5_SEQUENCES_BASE_HH__
