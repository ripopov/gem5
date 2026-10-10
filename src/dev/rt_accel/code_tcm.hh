/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __DEV_RT_ACCEL_CODE_TCM_HH__
#define __DEV_RT_ACCEL_CODE_TCM_HH__

#include "mem/abstract_mem.hh"
#include "mem/tport.hh"
#include "params/RTAccelCodeTCM.hh"

namespace gem5
{
namespace rt_accel
{

// Two independent pipelined read ports over one physical backing store.
// AbstractMemory supplies ELF loading, statistics and checkpoint storage;
// SimpleTimingPort supplies response queues, retries and draining.
class CodeTCM : public memory::AbstractMemory
{
  private:
    class ReadPort : public SimpleTimingPort
    {
      private:
        CodeTCM &memory;
        Tick recvAtomic(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        AddrRangeList getAddrRanges() const override;

      public:
        ReadPort(const std::string &name, CodeTCM &memory);
    };

    const Tick latency;
    ReadPort instruction, constants;

  public:
    using Params = RTAccelCodeTCMParams;
    explicit CodeTCM(const Params &p);
    void init() override;
    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
};

} // namespace rt_accel
} // namespace gem5
#endif
