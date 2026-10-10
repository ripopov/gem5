/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __DEV_RT_ACCEL_CONTROL_HH__
#define __DEV_RT_ACCEL_CONTROL_HH__

#include <deque>

#include "base/statistics.hh"
#include "dev/rt_accel/register_port.hh"
#include "params/RTAccelControl.hh"
#include "sim/clocked_object.hh"

namespace gem5
{
class BaseCPU;
namespace rt_accel
{

class Control : public ClockedObject
{
  private:
    class MasterPort : public RequestPort
    {
      private:
        Control &owner;

      public:
        PacketPtr pending = nullptr;
        bool blocked = false;
        Tick nextIssue = 0;
        MasterPort(Control &o, const std::string &suffix);
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
    };
    RegisterPort<Control> pio;
    MasterPort dma, dmaWrite, dmaClean;
    BaseCPU &cpu;
    const AddrRange registers, dataTcmRange, hostRange;
    const RequestorID requestor;
    const unsigned maxOutstanding;
    const bool coherentDma;
    uint64_t jobAddress = 0, msiAddress = 0;
    bool completionPending = false;
    uint32_t msiData = 0, status = 0;
    uint64_t source = 0, destination = 0;
    uint32_t length = 0, issued = 0, copied = 0, dmaStatus = 0;
    unsigned activeChunks = 0;
    bool dmaError = false;
    Tick dmaStart = 0;
    enum class Phase
    {
        Read,
        Clean,
        Write,
        MSI
    };
    struct ChunkState : Packet::SenderState
    {
        const Phase phase;
        const uint32_t offset;
        PacketPtr write = nullptr;
        ChunkState(Phase p, uint32_t o) : phase(p), offset(o) {}
        ~ChunkState() override
        {
            if (write) {
                delete write->popSenderState();
                delete write;
            }
        }
    };
    std::deque<PacketPtr> writes;
    std::deque<PacketPtr> cleans;
    struct DmaStats : statistics::Group
    {
        explicit DmaStats(Control &owner);
        statistics::Scalar readRequests, writeRequests, cleanRequests;
        statistics::Scalar bytesCopied;
        statistics::Scalar busyTicks, readRetries, writeRetries;
        statistics::Scalar peakChunks, errors;
    } stats;
    EventFunctionWrapper stepEvent;
    EventFunctionWrapper cleanEvent;
    void send();
    void sendClean();
    void kick();
    void sendPort(MasterPort &port);
    void readChunk();
    void response(PacketPtr pkt);
    void checkCompletion();
    bool validRange(Addr addr, uint32_t size) const;
    void finish(uint32_t code);

  public:
    using Params = RTAccelControlParams;
    explicit Control(const Params &p);
    void init() override;
    DrainState drain() override;
    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
    const AddrRange &
    range() const
    {
        return registers;
    }
    void access(PacketPtr pkt);
};

} // namespace rt_accel
} // namespace gem5
#endif
