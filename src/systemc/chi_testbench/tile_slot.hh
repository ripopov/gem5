/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SYSTEMC_CHI_TESTBENCH_TILE_SLOT_HH__
#define __SYSTEMC_CHI_TESTBENCH_TILE_SLOT_HH__

#include "params/ChiTileSlot.hh"
#include "sim/clocked_object.hh"

namespace gem5
{
namespace chi_testbench
{

/**
 * Minimal ClockedObject used as the `system.cpu[i]` placeholder when
 * there is no real CPU at a tile.
 *
 * CHI_RNF.__init__ attaches RubySequencer / L1 / L2 children to the
 * "cpu" object it is handed; those children need a proper gem5
 * SimObject parent so their stats register in a unique namespace.
 * A SystemC_ScModule cannot fill this role because its C++ base is
 * sc_module, not SimObject — its children would all collide at global
 * scope. ChiTileSlot exists purely to provide that parent.
 */
class ChiTileSlot : public ClockedObject
{
  public:
    using Params = ChiTileSlotParams;
    explicit ChiTileSlot(const Params &p) : ClockedObject(p) {}
};

} // namespace chi_testbench
} // namespace gem5

#endif // __SYSTEMC_CHI_TESTBENCH_TILE_SLOT_HH__
