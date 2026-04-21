/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SYSTEMC_CHI_TESTBENCH_OPCODE_WALK_HH__
#define __SYSTEMC_CHI_TESTBENCH_OPCODE_WALK_HH__

#include "params/OpcodeWalkDriver.hh"
#include "systemc/chi_testbench/driver_base.hh"

namespace gem5
{
namespace chi_testbench
{

/**
 * Scripted LD/ST sequence with data-dependent assertions. Exercises
 * the core CHI opcode envelope that a sequencer-side driver can
 * reach (ReadShared, ReadUnique, WriteBackFull, Evict) and verifies
 * that data written to a line is what comes back — both on the hot
 * path (read immediately after write) and on the cold path (read
 * after eviction via capacity pressure).
 *
 * Failure mode: any data mismatch calls panic() from the SC_THREAD.
 */
class OpcodeWalkDriver : public ChiDriverBase
{
  public:
    using Params = OpcodeWalkDriverParams;
    OpcodeWalkDriver(const Params &p, const sc_core::sc_module_name &mn);

  protected:
    void run() override;

  private:
    const uint64_t target_addr;
    const uint64_t filler_base;
    const uint32_t filler_lines;
    const uint32_t assertions_expected;
};

} // namespace chi_testbench
} // namespace gem5

#endif // __SYSTEMC_CHI_TESTBENCH_OPCODE_WALK_HH__
