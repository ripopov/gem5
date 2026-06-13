/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_RUBY_NETWORK_SIMPLE_XP_XPLINK_HH__
#define __MEM_RUBY_NETWORK_SIMPLE_XP_XPLINK_HH__

#include "mem/ruby/network/simple/SimpleLink.hh"
#include "params/XPExtLink.hh"
#include "params/XPIntLink.hh"

namespace gem5
{

namespace ruby
{

class XPExtLink : public SimpleExtLink
{
  public:
    PARAMS(XPExtLink);
    XPExtLink(const Params &p);
};

class XPIntLink : public SimpleIntLink
{
  public:
    PARAMS(XPIntLink);
    XPIntLink(const Params &p);
};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_SIMPLE_XP_XPLINK_HH__
