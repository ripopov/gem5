/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CHI_TESTBENCH_GEM5_SEQUENCES_IDLE_HH__
#define __CHI_TESTBENCH_GEM5_SEQUENCES_IDLE_HH__

#include "chi_testbench_gem5/sequences/base.hh"
#include "params/IdleSequence.hh"

namespace gem5
{
namespace chi_gem5tb
{

class IdleSequence : public ChiSequence
{
  public:
    using Params = IdleSequenceParams;
    explicit IdleSequence(const Params &p) : ChiSequence(p) {}
    void run(ChiSeqDriver &drv) override;
};

} // namespace chi_gem5tb
} // namespace gem5

#endif // __CHI_TESTBENCH_GEM5_SEQUENCES_IDLE_HH__
