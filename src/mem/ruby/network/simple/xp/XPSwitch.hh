/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_RUBY_NETWORK_SIMPLE_XP_XPSWITCH_HH__
#define __MEM_RUBY_NETWORK_SIMPLE_XP_XPSWITCH_HH__

#include <deque>
#include <string>
#include <vector>

#include "mem/ruby/common/Consumer.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/network/simple/Switch.hh"
#include "mem/ruby/slicc_interface/Message.hh"
#include "params/XPSwitch.hh"

namespace gem5
{

namespace ruby
{

class XPSwitch : public Switch, public Consumer
{
  public:
    static constexpr Event::Priority XP_EV_PRI = Event::Default_Pri;

    PARAMS(XPSwitch);

    XPSwitch(const Params &p);
    void init() override;

    void addXPInPort(const std::vector<MessageBuffer*>& in);
    void addXPOutPort(std::string link_name,
                      const std::vector<MessageBuffer*>& out,
                      const NetDest& routing_table_entry,
                      Cycles link_latency, int link_weight,
                      int bw_multiplier, bool is_external,
                      PortDirection dst_inport = "");

    void wakeup() override;
    void storeEventInfo(int info) override;
    void print(std::ostream& out) const override;

    bool functionalRead(Packet *pkt) override;
    bool functionalRead(Packet *pkt, WriteMask &mask) override;
    uint32_t functionalWrite(Packet *pkt) override;

    bool hasCustomStats() const override { return true; }
    const statistics::Formula &getLinkUtilization() const override;
    const statistics::Formula &getMsgCount(unsigned int type) const override;
    const statistics::Formula &getMsgBytes(unsigned int type) const override;

  private:
    // Consumer-side out-of-order adapter: a switch-local random-access
    // container of matured messages drained, in arrival order, from one
    // credited/in-order input MessageBuffer. The switch arbitrates
    // out-of-order over `ready`, then returns the credit to `source` on grant.
    // See MessageBufferCredited.md ("consumer-side OoO adapter").
    struct ReadyQueue
    {
        MessageBuffer *source = nullptr;
        std::deque<MsgPtr> ready;
        // Reorder-window bound (0 == unbounded). Mirrors source->getMaxSize()
        // so admission backpressure on `source` still bounds in-flight traffic
        // when the input link is uncredited.
        size_t capacity = 0;
        // Grants issued from this (input, vnet) this cycle; capped at the
        // per-vnet channel count to model input crossbar bandwidth.
        unsigned grantsThisCycle = 0;
    };

    struct InputPort
    {
        std::vector<ReadyQueue> vnets;
    };

    struct OutputPort
    {
        std::string name;
        Tick routingLatency;
        Cycles linkLatency;
        std::vector<MessageBuffer*> staging;
        std::vector<MessageBuffer*> downstream;
    };

    struct Candidate
    {
        ReadyQueue *queue = nullptr;
        size_t index = 0;
        MsgPtr msg;
        std::vector<BaseRoutingUnit::RouteInfo> routes;
        bool valid = false;
        bool oooSkip = false;
    };

    enum class DriveResult
    {
        Sent,
        Idle,
        CreditBlocked,
        OutputBlocked,
    };

    void resetGrantBudget(Tick current_time);
    void drainInputs(Tick current_time);
    void operateVnet(int vnet);
    bool driveLinks();
    DriveResult driveOutput(OutputPort &out_port, int vnet);
    Candidate selectCandidate(int vnet, int output);
    bool routesToOutput(const Message &msg, int vnet, int output,
                        std::vector<BaseRoutingUnit::RouteInfo> &routes);
    bool stagingAvailable(
        const std::vector<BaseRoutingUnit::RouteInfo> &routes,
        int vnet, Tick current_time) const;
    void grantCandidate(Candidate &candidate, int vnet, Tick current_time);
    void recordSend(const Message &msg, int vnet, Tick wait_time);
    int getChannelCnt(int vnet) const;

    const bool m_enable_ooo_pop;
    Tick m_grant_cycle = MaxTick;

    std::vector<InputPort> m_in;
    std::vector<OutputPort> m_out;
    std::vector<MessageBuffer*> m_portBuffers;
    unsigned m_numConnectedBuffers = 0;

    struct XPStats : public statistics::Group
    {
        XPStats(XPSwitch *parent);
        void regStats() override;

        statistics::Scalar grants;
        statistics::Scalar holSkips;
        statistics::Scalar stagingFullCycles;
        statistics::Scalar creditStallCycles;
        statistics::Scalar outputBlockedCycles;
        statistics::Scalar linkSends;
        statistics::Scalar accLinkUtilization;
        statistics::Scalar totalMsgWaitTime;
        statistics::Formula percentLinksUtilized;
        statistics::Formula avgMsgWaitTime;

        std::vector<statistics::Scalar *> msgTypeCounts;
        std::vector<statistics::Formula *> msgCounts;
        std::vector<statistics::Formula *> msgBytes;
    } xpStats;
};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_SIMPLE_XP_XPSWITCH_HH__
