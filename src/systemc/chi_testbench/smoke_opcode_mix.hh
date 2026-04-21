/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SYSTEMC_CHI_TESTBENCH_SMOKE_OPCODE_MIX_HH__
#define __SYSTEMC_CHI_TESTBENCH_SMOKE_OPCODE_MIX_HH__

#include <vector>

#include "params/SmokeOpcodeMixDriver.hh"
#include "systemc/chi_testbench/driver_base.hh"

namespace gem5
{
namespace chi_testbench
{

/**
 * Per-tile driver that mixes reads and writes over a configurable
 * address list, with a configurable read ratio. Designed to be
 * instantiated on every tile simultaneously so the full CHI opcode
 * envelope (ReadShared, ReadUnique, WriteBackFull, Evict, snoops)
 * exercises naturally.
 *
 * Uses a per-instance linear-congruential RNG so runs are deterministic
 * given (tile_id, seed_base).
 */
class SmokeOpcodeMixDriver : public ChiDriverBase
{
  public:
    using Params = SmokeOpcodeMixDriverParams;
    SmokeOpcodeMixDriver(const Params &p, const sc_core::sc_module_name &mn);

  protected:
    void run() override;

  private:
    const std::vector<uint64_t> addresses;
    const uint32_t access_size;
    const uint32_t iterations;
    const uint32_t percent_reads;
    const uint32_t seed;
    const bool stop_on_finish;
};

} // namespace chi_testbench
} // namespace gem5

#endif // __SYSTEMC_CHI_TESTBENCH_SMOKE_OPCODE_MIX_HH__
