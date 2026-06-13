/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_RUBY_NETWORK_SIMPLE_XP_XPNETWORK_HH__
#define __MEM_RUBY_NETWORK_SIMPLE_XP_XPNETWORK_HH__

#include "mem/ruby/network/simple/SimpleNetwork.hh"
#include "params/XPNetwork.hh"

namespace gem5
{

namespace ruby
{

class XPNetwork : public SimpleNetwork
{
  public:
    PARAMS(XPNetwork);

    XPNetwork(const Params &p);

    void makeExtOutLink(SwitchID src, NodeID dest, BasicLink* link,
                        std::vector<NetDest>& routing_table_entry) override;
    void makeExtInLink(NodeID src, SwitchID dest, BasicLink* link,
                       std::vector<NetDest>& routing_table_entry) override;
    void makeInternalLink(SwitchID src, SwitchID dest, BasicLink* link,
                          std::vector<NetDest>& routing_table_entry,
                          PortDirection src_outport,
                          PortDirection dst_inport) override;

    void print(std::ostream& out) const override;
};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_SIMPLE_XP_XPNETWORK_HH__
