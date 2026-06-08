/*
 * Copyright (c) 2026 The Regents of the University of California
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __MEM_RAMULATOR2_HH__
#define __MEM_RAMULATOR2_HH__

#include <deque>
#include <unordered_map>

#include "mem/abstract_mem.hh"
#include "params/Ramulator2.hh"

namespace Ramulator
{

class IFrontEnd;
class IMemorySystem;

} // namespace Ramulator

namespace gem5
{

namespace memory
{

class Ramulator2 : public AbstractMemory
{
  private:
    class MemoryPort : public ResponsePort
    {
      private:
        Ramulator2& mem;

      public:
        MemoryPort(const std::string& name, Ramulator2& memory);

      protected:
        Tick recvAtomic(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        bool recvTimingReq(PacketPtr pkt) override;
        void recvRespRetry() override;
        AddrRangeList getAddrRanges() const override;
    };

    MemoryPort port;

    std::string ramulatorConfig;
    Ramulator::IFrontEnd* frontend;
    Ramulator::IMemorySystem* memorySystem;

    bool retryReq;
    bool retryResp;
    Tick startTick;

    std::unordered_map<Addr, std::deque<PacketPtr>> outstandingReads;
    std::unordered_map<Addr, std::deque<PacketPtr>> outstandingWrites;
    unsigned int nbrOutstandingReads;
    unsigned int nbrOutstandingWrites;

    std::deque<PacketPtr> responseQueue;

    EventFunctionWrapper sendResponseEvent;
    EventFunctionWrapper tickEvent;

    std::unique_ptr<Packet> pendingDelete;

    unsigned int nbrOutstanding() const;
    void accessAndRespond(PacketPtr pkt);
    void sendResponse();
    void tick();

  public:
    typedef Ramulator2Params Params;

    Ramulator2(const Params& p);
    ~Ramulator2();

    DrainState drain() override;
    Port& getPort(const std::string& if_name,
                  PortID idx = InvalidPortID) override;
    void init() override;
    void startup() override;
    void resetStats() override;

  protected:
    Tick recvAtomic(PacketPtr pkt);
    void recvFunctional(PacketPtr pkt);
    bool recvTimingReq(PacketPtr pkt);
    void recvRespRetry();
};

} // namespace memory
} // namespace gem5

#endif // __MEM_RAMULATOR2_HH__
