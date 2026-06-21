/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mem/ruby/network/simple/xp/XPSwitch.hh"

#include <algorithm>

#include "base/cast.hh"
#include "base/cprintf.hh"
#include "debug/RubyNetwork.hh"
#include "mem/ruby/common/NetDest.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/network/simple/SimpleNetwork.hh"
#include "mem/ruby/slicc_interface/Message.hh"
#include "sim/stats.hh"

namespace gem5
{

namespace ruby
{

XPSwitch::XPSwitch(const Params &p)
    : Switch(p), Consumer(this, XP_EV_PRI),
      m_enable_ooo_pop(p.enable_ooo_pop), xpStats(this)
{
    m_portBuffers.reserve(p.port_buffers.size());
    for (auto &buffer : p.port_buffers) {
        m_portBuffers.push_back(buffer);
    }
}

void
XPSwitch::init()
{
    Switch::init();
}

void
XPSwitch::addXPInPort(const std::vector<MessageBuffer*>& in)
{
    const NodeID port = m_in.size();
    m_in.emplace_back();
    InputPort &in_port = m_in.back();
    in_port.vnets.resize(in.size());

    for (int vnet = 0; vnet < in.size(); ++vnet) {
        MessageBuffer *buffer = in[vnet];
        if (buffer == nullptr) {
            continue;
        }
        buffer->setConsumer(this);
        buffer->setIncomingLink(port);
        buffer->setVnet(vnet);

        ReadyQueue &queue = in_port.vnets[vnet];
        queue.source = buffer;
        // Bound the reorder window to the input buffer's capacity so that,
        // when uncredited, a full window leaves messages parked in `source`
        // and its slot-based backpressure still throttles the producer.
        queue.capacity = buffer->getMaxSize();
    }
}

void
XPSwitch::addXPOutPort(std::string link_name,
                       const std::vector<MessageBuffer*>& out,
                       const NetDest& routing_table_entry,
                       Cycles link_latency, int link_weight,
                       int bw_multiplier, bool is_external,
                       PortDirection dst_inport)
{
    std::vector<MessageBuffer*> staging;
    staging.reserve(out.size());
    for (int vnet = 0; vnet < out.size(); ++vnet) {
        assert(m_numConnectedBuffers < m_portBuffers.size());
        MessageBuffer *buffer = m_portBuffers[m_numConnectedBuffers++];
        buffer->setConsumer(this);
        buffer->setIncomingLink(m_out.size());
        buffer->setVnet(vnet);
        staging.push_back(buffer);
    }

    Tick routing_latency = is_external ?
        cyclesToTicks(params().ext_routing_latency) :
        cyclesToTicks(params().int_routing_latency);

    getRoutingUnit().addOutPort(m_out.size(), out, routing_table_entry,
                                dst_inport, link_weight);
    m_out.push_back({link_name, routing_latency, link_latency, staging, out});
}

void
XPSwitch::wakeup()
{
    Tick current_time = clockEdge();
    resetGrantBudget(current_time);

    // 1. Drive matured staging messages onto the downstream links.
    bool retry = driveLinks();

    // 2. Pull matured messages out of the in-order input buffers into the
    //    per-input random-access ready containers (returns no credit yet).
    drainInputs(current_time);

    // 3. Arbitrate the ready containers into the output staging buffers.
    for (int vnet = params().virt_nets - 1; vnet >= 0; --vnet) {
        operateVnet(vnet);
    }

    if (retry) {
        scheduleEvent(Cycles(1));
    }
}

void
XPSwitch::resetGrantBudget(Tick current_time)
{
    if (m_grant_cycle == current_time) {
        return;
    }
    m_grant_cycle = current_time;
    for (auto &in_port : m_in) {
        for (auto &queue : in_port.vnets) {
            queue.grantsThisCycle = 0;
        }
    }
}

void
XPSwitch::drainInputs(Tick current_time)
{
    for (auto &in_port : m_in) {
        for (auto &queue : in_port.vnets) {
            if (queue.source == nullptr) {
                continue;
            }
            // In-order (head-only) consumption of the input buffer; the
            // out-of-order choice is made later, over `ready`.
            while ((queue.capacity == 0 ||
                    queue.ready.size() < queue.capacity) &&
                   queue.source->isReady(current_time)) {
                queue.ready.push_back(queue.source->peekMsgPtr());
                queue.source->dequeue(current_time);
            }
        }
    }
}

void
XPSwitch::storeEventInfo(int)
{
}

void
XPSwitch::print(std::ostream& out) const
{
    out << "[XPSwitch]";
}

// The ready containers hold in-flight messages drained from the input
// buffers but not yet forwarded, so they are a home for line data that
// functional access must reach. The base Switch handles the staging buffers;
// the input buffers themselves are reached via the network's
// m_int_link_buffers list (internal links) or the owning controller
// (external injection).
bool
XPSwitch::functionalRead(Packet *pkt)
{
    for (auto &in_port : m_in) {
        for (auto &queue : in_port.vnets) {
            for (auto &msg : queue.ready) {
                if (msg->functionalRead(pkt)) {
                    return true;
                }
            }
        }
    }
    return Switch::functionalRead(pkt);
}

bool
XPSwitch::functionalRead(Packet *pkt, WriteMask &mask)
{
    bool read = Switch::functionalRead(pkt, mask);
    for (auto &in_port : m_in) {
        for (auto &queue : in_port.vnets) {
            for (auto &msg : queue.ready) {
                if (msg->functionalRead(pkt, mask)) {
                    read = true;
                }
            }
        }
    }
    return read;
}

uint32_t
XPSwitch::functionalWrite(Packet *pkt)
{
    uint32_t num_functional_writes = Switch::functionalWrite(pkt);
    for (auto &in_port : m_in) {
        for (auto &queue : in_port.vnets) {
            for (auto &msg : queue.ready) {
                num_functional_writes += msg->functionalWrite(pkt);
            }
        }
    }
    return num_functional_writes;
}

const statistics::Formula&
XPSwitch::getLinkUtilization() const
{
    return xpStats.percentLinksUtilized;
}

const statistics::Formula&
XPSwitch::getMsgCount(unsigned int type) const
{
    return *(xpStats.msgCounts[type]);
}

const statistics::Formula&
XPSwitch::getMsgBytes(unsigned int type) const
{
    return *(xpStats.msgBytes[type]);
}

void
XPSwitch::operateVnet(int vnet)
{
    Tick current_time = clockEdge();
    const int grants = std::max(1, getChannelCnt(vnet));

    for (int output = 0; output < m_out.size(); ++output) {
        for (int channel = 0; channel < grants; ++channel) {
            Candidate candidate = selectCandidate(vnet, output);
            if (!candidate.valid) {
                break;
            }
            grantCandidate(candidate, vnet, current_time);
        }
    }
}

bool
XPSwitch::driveLinks()
{
    bool retry = false;
    int total_slots = 0;
    int used_slots = 0;

    for (auto &out_port : m_out) {
        for (int vnet = 0; vnet < out_port.staging.size(); ++vnet) {
            int channels = getChannelCnt(vnet);
            total_slots += channels;
            for (int channel = 0; channel < channels; ++channel) {
                DriveResult result = driveOutput(out_port, vnet);
                if (result == DriveResult::Sent) {
                    used_slots++;
                    retry = true;
                    continue;
                }

                // No credit-return callback exists, so a credit-blocked
                // output must re-poll next cycle to notice returned credits.
                if (result == DriveResult::OutputBlocked ||
                    result == DriveResult::CreditBlocked) {
                    retry = true;
                }
                break;
            }
        }
    }

    if (total_slots != 0) {
        xpStats.accLinkUtilization +=
            static_cast<double>(used_slots) / static_cast<double>(total_slots);
    }

    return retry;
}

XPSwitch::DriveResult
XPSwitch::driveOutput(OutputPort &out_port, int vnet)
{
    if (vnet >= out_port.staging.size() ||
        vnet >= out_port.downstream.size()) {
        return DriveResult::Idle;
    }

    MessageBuffer *staging = out_port.staging[vnet];
    MessageBuffer *downstream = out_port.downstream[vnet];
    if (staging == nullptr || downstream == nullptr) {
        return DriveResult::Idle;
    }

    Tick current_time = clockEdge();
    if (!staging->isReady(current_time)) {
        return DriveResult::Idle;
    }

    if (!downstream->areNSlotsAvailable(1, current_time)) {
        if (downstream->isCredited() && !downstream->hasCredit()) {
            xpStats.creditStallCycles++;
            return DriveResult::CreditBlocked;
        }
        xpStats.outputBlockedCycles++;
        return DriveResult::OutputBlocked;
    }

    MsgPtr msg_ptr = staging->peekMsgPtr();
    Tick msg_enqueue_time = msg_ptr->getLastEnqueueTime();
    Message *msg = msg_ptr.get();

    staging->dequeue(current_time);
    downstream->enqueue(msg_ptr, current_time,
                        cyclesToTicks(out_port.linkLatency),
                        getNetPtr()->getRandomization(),
                        getNetPtr()->getWarmupEnabled());

    recordSend(*msg, vnet, current_time - msg_enqueue_time);
    return DriveResult::Sent;
}

XPSwitch::Candidate
XPSwitch::selectCandidate(int vnet, int output)
{
    Candidate selected;
    Tick current_time = clockEdge();
    const unsigned channel_cnt = getChannelCnt(vnet);

    for (auto &in_port : m_in) {
        if (vnet >= in_port.vnets.size()) {
            continue;
        }

        ReadyQueue &queue = in_port.vnets[vnet];
        if (queue.source == nullptr || queue.ready.empty() ||
            queue.grantsThisCycle >= channel_cnt) {
            continue;
        }

        // Scan the matured messages oldest-first for the oldest one that
        // routes to `output` and whose downstream staging has room. With OoO
        // disabled only the head (front) of the container is eligible.
        const size_t limit = m_enable_ooo_pop ? queue.ready.size() : 1;
        bool eligible_found = false;
        bool routing_match_staging_full = false;

        for (size_t i = 0; i < limit; ++i) {
            const MsgPtr &msg_ptr = queue.ready[i];
            std::vector<BaseRoutingUnit::RouteInfo> routes;
            if (!routesToOutput(*msg_ptr, vnet, output, routes)) {
                continue;
            }
            if (!stagingAvailable(routes, vnet, current_time)) {
                routing_match_staging_full = true;
                continue;
            }

            eligible_found = true;
            if (!selected.valid || selected.msg > msg_ptr) {
                selected.queue = &queue;
                selected.index = i;
                selected.msg = msg_ptr;
                selected.valid = true;
                selected.oooSkip = i != 0;
                selected.routes = std::move(routes);
            }
            break;
        }

        if (!eligible_found && routing_match_staging_full) {
            xpStats.stagingFullCycles++;
        }
    }

    return selected;
}

bool
XPSwitch::routesToOutput(const Message &msg, int vnet, int output,
                         std::vector<BaseRoutingUnit::RouteInfo> &routes)
{
    routes.clear();
    getRoutingUnit().route(msg, vnet, getNetPtr()->isVNetOrdered(vnet),
                           routes);

    for (const auto &route : routes) {
        if (route.m_link_id == output) {
            return true;
        }
    }
    return false;
}

bool
XPSwitch::stagingAvailable(
    const std::vector<BaseRoutingUnit::RouteInfo> &routes,
    int vnet, Tick current_time) const
{
    for (const auto &route : routes) {
        assert(route.m_link_id < m_out.size());
        const OutputPort &out_port = m_out[route.m_link_id];
        if (vnet >= out_port.staging.size() ||
            out_port.staging[vnet] == nullptr ||
            !out_port.staging[vnet]->areNSlotsAvailable(1, current_time)) {
            return false;
        }
    }
    return true;
}

void
XPSwitch::grantCandidate(Candidate &candidate, int vnet, Tick current_time)
{
    ReadyQueue &queue = *candidate.queue;
    MessageBuffer *source = queue.source;

    // Remove the granted message from the ready container by position. This is
    // invalidation-proof because exactly one candidate is live at a time: the
    // caller re-selects after every grant.
    assert(candidate.index < queue.ready.size());
    MsgPtr msg_ptr = queue.ready[candidate.index];
    queue.ready.erase(queue.ready.begin() + candidate.index);
    queue.grantsThisCycle++;

    // The message has departed the switch's input stage, so the slot it
    // occupied upstream is now free: return the credit after the link latency.
    if (source->isCredited()) {
        source->returnCredit(current_time,
                             cyclesToTicks(source->creditReturnLatency()));
    }

    xpStats.grants++;
    if (candidate.oooSkip) {
        xpStats.holSkips++;
    }

    MsgPtr unmodified_msg_ptr;
    if (candidate.routes.size() > 1) {
        unmodified_msg_ptr = msg_ptr->clone();
    }

    for (int i = 0; i < candidate.routes.size(); ++i) {
        const int outgoing = candidate.routes[i].m_link_id;
        assert(outgoing < m_out.size());
        OutputPort &out_port = m_out[outgoing];
        assert(vnet < out_port.staging.size());
        assert(out_port.staging[vnet] != nullptr);

        if (i > 0) {
            msg_ptr = unmodified_msg_ptr->clone();
        }

        Message *msg = msg_ptr.get();
        msg->getDestination() = candidate.routes[i].m_destinations;

        DPRINTF(RubyNetwork, "XP enqueue inport[%d][%d] to "
                "outport[%d][%d]\n",
                source->getIncomingLink(), vnet, outgoing, vnet);

        out_port.staging[vnet]->enqueue(
            msg_ptr, current_time, out_port.routingLatency,
            getNetPtr()->getRandomization(),
            getNetPtr()->getWarmupEnabled());
    }
}

void
XPSwitch::recordSend(const Message &msg, int vnet, Tick wait_time)
{
    const auto type = msg.getMessageSize();
    *(xpStats.msgTypeCounts[type]) += 1;
    xpStats.linkSends++;
    xpStats.totalMsgWaitTime += wait_time;
}

int
XPSwitch::getChannelCnt(int vnet) const
{
    const std::vector<int> &channels =
        getNetPtr()->params().physical_vnets_channels;
    if (channels.empty()) {
        return 1;
    }
    assert(vnet < channels.size());
    assert(channels[vnet] > 0);
    return channels[vnet];
}

XPSwitch::XPStats::XPStats(XPSwitch *parent)
    : statistics::Group(parent, "xp"),
      ADD_STAT(grants, statistics::units::Count::get(),
               "Messages granted from XP input buffers into staging"),
      ADD_STAT(holSkips, statistics::units::Count::get(),
               "HoL-eliminating grants that skipped the physical head"),
      ADD_STAT(stagingFullCycles, statistics::units::Cycle::get(),
               "Cycles where ready input traffic found staging full"),
      ADD_STAT(creditStallCycles, statistics::units::Cycle::get(),
               "Cycles where a ready staging message lacked link credit"),
      ADD_STAT(outputBlockedCycles, statistics::units::Cycle::get(),
               "Cycles where a ready staging message found output full"),
      ADD_STAT(linkSends, statistics::units::Count::get(),
               "Messages sent by XP link drivers"),
      ADD_STAT(accLinkUtilization, statistics::units::Count::get(),
               "Accumulated XP link utilization samples"),
      ADD_STAT(totalMsgWaitTime, statistics::units::Tick::get(),
               "Total ticks messages spent waiting in staging"),
      ADD_STAT(percentLinksUtilized, statistics::units::Ratio::get(),
               "Percent utilization of XP link slots"),
      ADD_STAT(avgMsgWaitTime, statistics::units::Ratio::get(),
               "Average ticks a message spent waiting in staging")
{
    grants.flags(statistics::nozero);
    holSkips.flags(statistics::nozero);
    stagingFullCycles.flags(statistics::nozero);
    creditStallCycles.flags(statistics::nozero);
    outputBlockedCycles.flags(statistics::nozero);
    linkSends.flags(statistics::nozero);
    accLinkUtilization.flags(statistics::nozero);
    totalMsgWaitTime.flags(statistics::nozero);
    percentLinksUtilized.flags(statistics::nozero);
    avgMsgWaitTime.flags(statistics::nozero | statistics::nonan);

    percentLinksUtilized =
        100 * accLinkUtilization /
        (simTicks / statistics::constant(parent->clockPeriod()));
    avgMsgWaitTime = totalMsgWaitTime / linkSends;

    for (unsigned int type = MessageSizeType_FIRST; type < MessageSizeType_NUM;
         ++type) {
        std::string name_count = csprintf(
            "msg_count.%s", MessageSizeType_to_string(MessageSizeType(type)));
        auto raw_count = new statistics::Scalar(
            this, csprintf("raw_%s", name_count).c_str(),
            statistics::units::Count::get(),
            "Total messages through XP switch");
        raw_count->flags(statistics::nozero);

        auto formula_count = new statistics::Formula(
            this, name_count.c_str(),
            statistics::units::Count::get(),
            "Total messages through XP switch");
        formula_count->flags(statistics::nozero);
        *formula_count = *raw_count;

        auto formula_bytes = new statistics::Formula(
            this, csprintf("msg_bytes.%s",
                           MessageSizeType_to_string(MessageSizeType(type)))
                      .c_str(),
            statistics::units::Byte::get(),
            "Total bytes through XP switch");
        formula_bytes->flags(statistics::nozero);

        msgTypeCounts.push_back(raw_count);
        msgCounts.push_back(formula_count);
        msgBytes.push_back(formula_bytes);
    }
}

void
XPSwitch::XPStats::regStats()
{
    for (unsigned int type = MessageSizeType_FIRST; type < MessageSizeType_NUM;
         ++type) {
        *(msgBytes[type]) =
            *(msgCounts[type]) *
            statistics::constant(
                Network::MessageSizeType_to_int(MessageSizeType(type)));
    }

    statistics::Group::regStats();
}

} // namespace ruby
} // namespace gem5
