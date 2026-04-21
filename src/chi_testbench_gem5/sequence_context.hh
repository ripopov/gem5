/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CHI_TESTBENCH_GEM5_SEQUENCE_CONTEXT_HH__
#define __CHI_TESTBENCH_GEM5_SEQUENCE_CONTEXT_HH__

#include <functional>

namespace gem5
{
namespace chi_gem5tb
{

class ChiSeqDriver;

/**
 * Thin handle passed to every registered sequence function.
 *
 * Sequences reach parameters via `ctx.drv.get_<field>()` accessors on
 * the driver (kept as inline getters so the ChiSeqDriverParams struct
 * stays an implementation detail). Non-owning — the driver outlives
 * the sequence.
 */
struct SequenceContext
{
    ChiSeqDriver &drv;
};

using SequenceFn = std::function<void(SequenceContext &)>;

} // namespace chi_gem5tb
} // namespace gem5

#endif // __CHI_TESTBENCH_GEM5_SEQUENCE_CONTEXT_HH__
