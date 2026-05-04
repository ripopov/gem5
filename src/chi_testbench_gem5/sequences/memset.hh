/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CHI_TESTBENCH_GEM5_SEQUENCES_MEMSET_HH__
#define __CHI_TESTBENCH_GEM5_SEQUENCES_MEMSET_HH__

#include "chi_testbench_gem5/sequences/base.hh"
#include "params/MemsetSequence.hh"

namespace gem5
{
namespace chi_gem5tb
{

class MemsetSequence : public ChiSequence
{
  public:
    using Params = MemsetSequenceParams;
    explicit MemsetSequence(const Params &p) : ChiSequence(p), _p(p) {}
    void run(ChiSeqDriver &drv) override;

  private:
    const Params &_p;
};

} // namespace chi_gem5tb
} // namespace gem5

#endif // __CHI_TESTBENCH_GEM5_SEQUENCES_MEMSET_HH__
