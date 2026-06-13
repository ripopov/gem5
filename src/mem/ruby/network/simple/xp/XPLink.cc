/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mem/ruby/network/simple/xp/XPLink.hh"

namespace gem5
{

namespace ruby
{

XPExtLink::XPExtLink(const Params &p)
    : SimpleExtLink(p)
{
}

XPIntLink::XPIntLink(const Params &p)
    : SimpleIntLink(p)
{
}

} // namespace ruby
} // namespace gem5
