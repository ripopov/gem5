/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __DEV_RT_ACCEL_REGISTER_PORT_HH__
#define __DEV_RT_ACCEL_REGISTER_PORT_HH__

#include <deque>

#include "mem/port.hh"
#include "sim/eventq.hh"

namespace gem5
{
namespace rt_accel
{

// Bounded timing registers: sample only after crossbar delivery and one
// device clock, and hold responses until the upstream port accepts them.
template <class Device> class RegisterPort : public ResponsePort
{
  private:
    Device &owner;
    unsigned outstanding = 0;
    bool retry = false, blocked = false;
    std::deque<PacketPtr> responses;
    void
    sendResponses()
    {
        while (!responses.empty()) {
            if (!sendTimingResp(responses.front())) {
                blocked = true;
                return;
            }
            responses.pop_front();
            --outstanding;
            if (retry) {
                retry = false;
                sendRetryReq();
            }
        }
    }
    AddrRangeList
    getAddrRanges() const override
    {
        return {owner.range()};
    }
    bool
    recvTimingReq(PacketPtr pkt) override
    {
        if (outstanding == 16) {
            retry = true;
            return false;
        }
        ++outstanding;
        Tick when =
            owner.clockEdge(Cycles(1)) + pkt->headerDelay + pkt->payloadDelay;
        pkt->headerDelay = pkt->payloadDelay = 0;
        owner.schedule(new EventFunctionWrapper(
                           [this, pkt] {
                               owner.access(pkt);
                               responses.push_back(pkt);
                               if (!blocked) {
                                   sendResponses();
                               }
                           },
                           owner.name() + ".register", true),
                       when);
        return true;
    }
    void
    recvRespRetry() override
    {
        blocked = false;
        sendResponses();
    }
    Tick
    recvAtomic(PacketPtr) override
    {
        panic("RT control registers require timing accesses");
    }
    void
    recvFunctional(PacketPtr) override
    {
        panic("RT functional register accesses are unsupported");
    }

  public:
    explicit RegisterPort(Device &o)
        : ResponsePort(o.name() + ".pio"), owner(o)
    {}
};

} // namespace rt_accel
} // namespace gem5
#endif
