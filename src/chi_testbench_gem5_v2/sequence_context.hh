/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __CHI_TESTBENCH_GEM5_V2_SEQUENCE_CONTEXT_HH__
#define __CHI_TESTBENCH_GEM5_V2_SEQUENCE_CONTEXT_HH__

#include <functional>

namespace gem5
{
namespace chi_gem5tb_v2
{

class ChiDriverNode;

/**
 * Thin handle passed to every registered sequence function.
 *
 * Sequences reach parameters via `ctx.drv.get_<field>()` accessors on
 * the driver. Non-owning — the driver outlives the sequence.
 */
struct SequenceContext
{
    ChiDriverNode &drv;
};

using SequenceFn = std::function<void(SequenceContext &)>;

} // namespace chi_gem5tb_v2
} // namespace gem5

#endif // __CHI_TESTBENCH_GEM5_V2_SEQUENCE_CONTEXT_HH__
