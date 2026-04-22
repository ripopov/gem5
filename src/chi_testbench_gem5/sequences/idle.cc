/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "chi_testbench_gem5/sequences/idle.hh"

namespace gem5
{
namespace chi_gem5tb
{

// No traffic. Returns immediately; the driver signals the finish
// barrier (if present) and stays event-less until sim exit.
void
IdleSequence::run(ChiSeqDriver & /*drv*/)
{}

} // namespace chi_gem5tb
} // namespace gem5
