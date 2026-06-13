/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mem/ruby/network/simple/xp/XPNetwork.hh"

#include "base/cast.hh"
#include "mem/ruby/common/NetDest.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/network/simple/xp/XPLink.hh"
#include "mem/ruby/network/simple/xp/XPSwitch.hh"

namespace gem5
{

namespace ruby
{

XPNetwork::XPNetwork(const Params &p)
    : SimpleNetwork(p)
{
}

void
XPNetwork::makeExtOutLink(SwitchID src, NodeID global_dest,
                          BasicLink* link,
                          std::vector<NetDest>& routing_table_entry)
{
    NodeID local_dest = getLocalNodeID(global_dest);
    assert(local_dest < m_nodes);
    assert(m_switches[src] != nullptr);

    XPExtLink *xp_link = safe_cast<XPExtLink*>(link);
    XPSwitch *xp_switch = safe_cast<XPSwitch*>(m_switches[src]);

    int num_vnets = params().number_of_virtual_networks;
    gem5_assert(num_vnets >= m_fromNetQueues[local_dest].size(),
                "XPNetwork::makeExtOutLink");
    m_fromNetQueues[local_dest].resize(num_vnets, nullptr);

    xp_switch->addXPOutPort(xp_link->name(),
                            m_fromNetQueues[local_dest],
                            routing_table_entry[0],
                            xp_link->m_latency, 0,
                            xp_link->m_bw_multiplier, true);
}

void
XPNetwork::makeExtInLink(NodeID global_src, SwitchID dest,
                         BasicLink* link,
                         std::vector<NetDest>& routing_table_entry)
{
    NodeID local_src = getLocalNodeID(global_src);
    assert(local_src < m_nodes);
    assert(m_switches[dest] != nullptr);

    XPSwitch *xp_switch = safe_cast<XPSwitch*>(m_switches[dest]);
    xp_switch->addXPInPort(m_toNetQueues[local_src]);
}

void
XPNetwork::makeInternalLink(SwitchID src, SwitchID dest, BasicLink* link,
                            std::vector<NetDest>& routing_table_entry,
                            PortDirection src_outport,
                            PortDirection dst_inport)
{
    XPIntLink *xp_link = safe_cast<XPIntLink*>(link);
    XPSwitch *src_switch = safe_cast<XPSwitch*>(m_switches[src]);
    XPSwitch *dest_switch = safe_cast<XPSwitch*>(m_switches[dest]);

    dest_switch->addXPInPort(xp_link->m_buffers);
    src_switch->addXPOutPort(xp_link->name(), xp_link->m_buffers,
                             routing_table_entry[0], xp_link->m_latency,
                             xp_link->m_weight, xp_link->m_bw_multiplier,
                             false, dst_inport);

    m_int_link_buffers.insert(m_int_link_buffers.end(),
                              xp_link->m_buffers.begin(),
                              xp_link->m_buffers.end());
}

void
XPNetwork::print(std::ostream& out) const
{
    out << "[XPNetwork]";
}

} // namespace ruby
} // namespace gem5
