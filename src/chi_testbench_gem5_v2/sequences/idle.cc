/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5_v2/driver.hh"
#include "chi_testbench_gem5_v2/sequence_context.hh"
#include "chi_testbench_gem5_v2/sequences/registry.hh"

namespace gem5
{
namespace chi_gem5tb_v2
{
namespace
{

// Idle sequence: no CHI traffic. The fiber returns immediately; the
// driver signals the finish barrier (if present) and stays event-less
// until sim exit.
void
idle_seq(SequenceContext & /*ctx*/)
{}

[[maybe_unused]] Registrar _r("idle", &idle_seq);

} // namespace
} // namespace chi_gem5tb_v2
} // namespace gem5
