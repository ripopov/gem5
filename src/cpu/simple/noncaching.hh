/*
 * Copyright (c) 2012, 2018 ARM Limited
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
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

#ifndef __CPU_SIMPLE_NONCACHING_HH__
#define __CPU_SIMPLE_NONCACHING_HH__

#include "base/addr_range_map.hh"
#include "cpu/simple/atomic.hh"
#include "mem/abstract_mem.hh"
#include "mem/backdoor.hh"
#include "params/BaseNonCachingSimpleCPU.hh"

namespace gem5
{

/**
 * The NonCachingSimpleCPU is an AtomicSimpleCPU using the
 * 'atomic_noncaching' memory mode instead of just 'atomic'.
 */
class NonCachingSimpleCPU : public AtomicSimpleCPU
{
  public:
    NonCachingSimpleCPU(const BaseNonCachingSimpleCPUParams &p);

    void verifyMemoryMode() const override;
    void startup() override;
    void switchOut() override;
    void takeOverFrom(BaseCPU *old_cpu) override;

  protected:
    const std::vector<memory::AbstractMemory *> directMemory;
    struct DirectMapping
    {
        Addr start, end;
        uint8_t *base;
        std::vector<memory::AbstractMemory *> owners;
        bool writeable;
    };
    std::vector<DirectMapping> directMappings;
    void rebuildDirectMappings();
    bool directAccessActive() const;

    AddrRangeMap<MemBackdoorPtr, 1> memBackdoors;

    /**
     * The direct mapping or backdoor used by the last fetch/data access,
     * as plain bounds and a host pointer. Consecutive accesses
     * almost always hit the same memory, so this is checked before the
     * range map and resolves an address with two compares and an add.
     */
    struct BackdoorWindow
    {
        MemBackdoorPtr backdoor = nullptr;
        Addr start = 0;
        Addr end = 0; // exclusive
        uint8_t *base = nullptr; // host address of start
        bool readable = false;
        bool writeable = false;
        const DirectMapping *direct = nullptr;

        void
        set(MemBackdoorPtr bd)
        {
            backdoor = bd;
            start = bd->range().start();
            end = bd->range().end();
            base = bd->ptr();
            readable = bd->readable();
            writeable = bd->writeable();
        }

        void forget() { *this = BackdoorWindow(); }
    };
    BackdoorWindow fetchWindow, dataWindow;

    /**
     * The instruction page being executed from, as a host pointer.
     * While the fetch PC stays inside it and the TLB's translation epoch
     * is unchanged, a fetch is a copy from this pointer with no request,
     * translation or backdoor lookup. The bytes are read afresh on every
     * fetch, so writes to the page need no special handling; only the
     * translation is cached, and only for pages the TLB promises are
     * uniformly translated (BaseTLB::stableFetchPage()).
     */
    struct FetchPage
    {
        Addr vpage = 0;
        Addr size = 0; // zero: nothing cached
        uint64_t epoch = 0;
        const uint8_t *host = nullptr;
        MemBackdoorPtr backdoor = nullptr;
    } fetchPage;

    Fault fetchInstruction(Tick &latency) override;

    /**
     * Host address of [addr, addr + size) through an eligible direct mapping
     * or recorded backdoor, or nullptr. Refreshes the window when the
     * access is outside it.
     */
    uint8_t *hostAddr(BackdoorWindow &window, Addr addr, unsigned size,
                      bool write);

    /**
     * Perform a plain load or store through a recorded backdoor instead
     * of the port, if the packet and the target allow it.
     *
     * @return true if the packet was completed here.
     */
    bool tryBackdoorAccess(const PacketPtr &pkt);

    /** May plain stores skip the port? See tryBackdoorAccess(). */
    bool storesBypassPort() const;

    /**
     * Is this a plain load or store the fast path in readMem()/writeMem()
     * may handle: one cache-line fragment, every byte enabled, no
     * reservation, atomic, prefetch or cache-maintenance semantics?
     */
    bool plainAccess(Addr addr, unsigned size, Request::Flags flags,
                     const std::vector<bool> &byte_enable) const;

    Tick sendPacket(RequestPort &port, const PacketPtr &pkt) override;
    Tick fetchInstMem() override;

    Fault readMem(Addr addr, uint8_t *data, unsigned size,
                  Request::Flags flags,
                  const std::vector<bool> &byte_enable) override;
    Fault writeMem(uint8_t *data, unsigned size, Addr addr,
                   Request::Flags flags, uint64_t *res,
                   const std::vector<bool> &byte_enable) override;
};

} // namespace gem5

#endif // __CPU_SIMPLE_NONCACHING_HH__
