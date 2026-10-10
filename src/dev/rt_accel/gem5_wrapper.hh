/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __DEV_RT_ACCEL_GEM5_WRAPPER_HH__
#define __DEV_RT_ACCEL_GEM5_WRAPPER_HH__

#include <deque>
#include <fstream>

#include "dev/rt_accel/transport.hh"
#include "mem/port.hh"
#include "params/RTGem5Accelerator.hh"
#include "sim/clocked_object.hh"

namespace gem5
{
namespace rt_accel
{

class HostAdapter;

class Gem5Wrapper : public ClockedObject, public TransportEndpoint
{
  private:
    std::ofstream trace;
    class MmioPort : public RequestPort
    {
      private:
        Gem5Wrapper &owner;

      public:
        explicit MmioPort(Gem5Wrapper &o);
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
    };
    class MemoryPort : public ResponsePort
    {
      private:
        Gem5Wrapper &owner;

      public:
        explicit MemoryPort(Gem5Wrapper &o);
        AddrRangeList getAddrRanges() const override;
        bool recvTimingReq(PacketPtr pkt) override;
        void recvRespRetry() override;
        Tick recvAtomic(PacketPtr) override;
        void recvFunctional(PacketPtr) override;
    };
    HostAdapter &host;
    const Tick requestDelay, responseDelay;
    const unsigned queueDepth;
    const Event::Priority priority;
    const AddrRangeList systemRanges;
    MmioPort mmioPort;
    MemoryPort memoryPort;
    std::deque<PacketPtr> mmioRequests, masterResponses;
    bool mmioBlocked = false, responseBlocked = false;
    bool masterRetry = false;
    unsigned outstanding = 0;
    void sendMmio();
    void sendResponses();

  public:
    using Params = RTGem5AcceleratorParams;
    explicit Gem5Wrapper(const Params &p);
    void init() override;
    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
    EventManager &
    manager() override
    {
        return *this;
    }
    Tick
    requestLatency() const override
    {
        return requestDelay;
    }
    Tick
    responseLatency() const override
    {
        return responseDelay;
    }
    void receiveMmio(PacketPtr pkt) override;
    void completeMaster(PacketPtr pkt) override;
    DrainState drain() override;
};

} // namespace rt_accel
} // namespace gem5
#endif
