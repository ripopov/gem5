/*
 * Copyright (c) 2026 ripopov
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_RUBY_NETWORK_SIMPLE_XP_XPSWITCH_HH__
#define __MEM_RUBY_NETWORK_SIMPLE_XP_XPSWITCH_HH__

#include <string>
#include <vector>

#include "mem/ruby/common/Consumer.hh"
#include "mem/ruby/network/simple/Switch.hh"
#include "mem/ruby/network/simple/xp/CreditedLinkBuffer.hh"
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

    bool hasCustomStats() const override { return true; }
    const statistics::Formula &getLinkUtilization() const override;
    const statistics::Formula &getMsgCount(unsigned int type) const override;
    const statistics::Formula &getMsgBytes(unsigned int type) const override;

  private:
    struct InputPort
    {
        std::vector<MessageBuffer*> buffers;
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
        MessageBuffer *buffer = nullptr;
        CreditedLinkBuffer *credited = nullptr;
        CreditedLinkBuffer::Handle creditedHandle;
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
