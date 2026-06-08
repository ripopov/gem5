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

#include "base/statistics.hh"
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

    // When true, writes are acknowledged to the requestor at write-buffer
    // enqueue (gem5 MemCtrl-style posted writes) rather than on DRAM
    // completion, so requestor-visible write back-pressure matches gem5.
    const bool postWrites;
    const Tick writeFrontendLatency;

    bool retryReq;
    bool retryResp;
    Tick startTick;

    std::unordered_map<Addr, std::deque<PacketPtr>> outstandingReads;
    std::unordered_map<Addr, std::deque<PacketPtr>> outstandingWrites;
    unsigned int nbrOutstandingReads;
    unsigned int nbrOutstandingWrites;

    // Enqueue tick of each in-flight write, kept per address so the
    // DRAM-completion callback can recover the true enqueue-to-commit
    // latency even when the requestor was already acknowledged (posted
    // writes). This is the Ramulator2 analogue of gem5 MemCtrl's
    // requestorWriteTotalLat (readyTime - entryTime).
    std::unordered_map<Addr, std::deque<Tick>> writeEnqueueTicks;

    std::deque<PacketPtr> responseQueue;

    EventFunctionWrapper sendResponseEvent;
    EventFunctionWrapper tickEvent;

    std::unique_ptr<Packet> pendingDelete;

    unsigned int nbrOutstanding() const;
    void accessAndRespond(PacketPtr pkt, Tick static_latency = 0);
    void recordWriteCompletion(Addr addr);
    void sendResponse();
    void tick();

    struct Ramulator2Stats : public statistics::Group
    {
        Ramulator2Stats(Ramulator2 &mem);
        // Writes whose DRAM commit completion callback has fired.
        statistics::Scalar writeCompletions;
        // Summed enqueue-to-commit latency (Ticks) of completed writes.
        statistics::Scalar totalWriteCompletionLatency;
        // Mean enqueue-to-commit write latency (Ticks).
        statistics::Formula avgWriteCompletionLatency;
    } ramStats;

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
